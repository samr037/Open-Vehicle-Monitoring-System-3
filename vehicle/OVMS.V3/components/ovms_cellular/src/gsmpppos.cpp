/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "ovms_log.h"
static const char *TAG = "gsm-ppp";

#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/dns.h>
#include "ovms_cellular.h"
#include "gsmpppos.h"
#include "ovms_command.h"
#include "ovms_config.h"
#include "ovms_events.h"
#include "ovms_netmanager.h"
#if ESP_IDF_VERSION_MAJOR >= 5
#include <netif/ppp/pppapi.h>
#endif

static u32_t GsmPPPOS_OutputCallback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx)
  {
  GsmPPPOS* me = (GsmPPPOS*)ctx;

  // This is called in LwIP (tiT task) context, i.e. on core #1 (cellular task on #0).
  // A final PPP termination request may result from an LwIP mbox timeout, potentially
  // concurrently to a running MUX shutdown, so lock & check MUX:
  OvmsMutexLock lock(&me->m_modem->m_mux_mutex);
  if (!me->m_mux) return 0;

  MyCommandApp.HexDump(TAG, "tx", (const char*)data, len);
  return me->m_mux->tx(me->m_channel, data, len);
  }

static void GsmPPPOS_StatusCallback(ppp_pcb *pcb, int err_code, void *ctx)
  {
  GsmPPPOS* me = (GsmPPPOS*)ctx;
  struct netif *pppif = ppp_netif(pcb);

  me->m_lasterrcode = err_code;
  ESP_LOGI(TAG, "StatusCallBack: %s",me->ErrCodeName(err_code));

  switch (err_code)
    {
    case PPPERR_NONE:
      {
      ESP_LOGI(TAG, "status_cb: Connected");
#if PPP_IPV4_SUPPORT
      ESP_LOGI(TAG, "   our_ipaddr  = %s", ipaddr_ntoa(&pppif->ip_addr));
      ESP_LOGI(TAG, "   his_ipaddr  = %s", ipaddr_ntoa(&pppif->gw));
      ESP_LOGI(TAG, "   netmask     = %s", ipaddr_ntoa(&pppif->netmask));
      const ip_addr_t* dns = dns_getserver(0);
      ESP_LOGI(TAG, "   DNS#0       = %s", ipaddr_ntoa(dns));
      dns = dns_getserver(1);
      ESP_LOGI(TAG, "   DNS#1       = %s", ipaddr_ntoa(dns));
#endif /* PPP_IPV4_SUPPORT */
#if PPP_IPV6_SUPPORT
      ESP_LOGI(TAG, "   our6_ipaddr = %s", ip6addr_ntoa(netif_ip6_addr(pppif, 0)));
#endif /* PPP_IPV6_SUPPORT */
      me->m_connected = true;
      me->m_connectcount++;
      MyEvents.SignalEvent("network.interface.up", NULL);
      MyEvents.SignalEvent("system.modem.gotip",NULL);
      return;
      }
    case PPPERR_PARAM:
      {
      ESP_LOGE(TAG, "status_cb: Invalid parameter");
      break;
      }
    case PPPERR_OPEN:
      {
      ESP_LOGE(TAG, "status_cb: Unable to open PPP session");
      break;
      }
    case PPPERR_DEVICE:
      {
      ESP_LOGE(TAG, "status_cb: Invalid I/O device for PPP");
      break;
      }
    case PPPERR_ALLOC:
      {
      ESP_LOGE(TAG, "status_cb: Unable to allocate resources");
      break;
      }
    case PPPERR_USER:
      {
      if (me->m_shutdown)
        {
        // Closed on our request (GsmPPPOS::Shutdown): the modem state machine
        // owns the restart, do not reconnect from here.
        if (me->m_connected)
          {
          ESP_LOGI(TAG, "PPP connection has been closed");
          me->m_connected = false;
          MyEvents.SignalEvent("system.modem.down",NULL);
          }
        return;
        }
      // lwIP sets pcb->err_code = PPPERR_USER in ppp_close() and only clears it
      // in sifup()/sif6up(), i.e. on the next successful IPCP. ppp_connect() does
      // not reset it. So every attempt that fails before getting an address after
      // a previous ppp_close() is reported as PPPERR_USER, not PPPERR_CONNECT.
      // Without a Shutdown() of ours this is a link loss: fall through to the
      // common reconnect path below. (Seen as a 45 h LTE outage with the modem
      // sitting in NetMode, and as upstream issue #1513.)
      ESP_LOGW(TAG, "status_cb: PPP closed without a shutdown request - treating as link loss");
      break;
      }
    case PPPERR_CONNECT:
      {
      ESP_LOGE(TAG, "status_cb: Connection lost");
      break;
      }
    case PPPERR_AUTHFAIL:
      {
      ESP_LOGE(TAG, "status_cb: Failed authentication challenge");
      break;
      }
    case PPPERR_PROTOCOL:
      {
      ESP_LOGE(TAG, "status_cb: Failed to meet protocol");
      break;
      }
    case PPPERR_PEERDEAD:
      {
      ESP_LOGE(TAG, "status_cb: Connection timeout");
      break;
      }
    case PPPERR_IDLETIMEOUT:
      {
      ESP_LOGE(TAG, "status_cb: Idle Timeout");
      break;
      }
    case PPPERR_CONNECTTIME:
      {
      ESP_LOGE(TAG, "status_cb: Max connect time reached");
      break;
      }
    case PPPERR_LOOPBACK:
      {
      ESP_LOGE(TAG, "status_cb: Loopback detected");
      break;
      }
    default:
      {
      ESP_LOGE(TAG, "status_cb: Unknown error code %d", err_code);
      break;
      }
    }

  ESP_LOGI(TAG, "Shutdown (via status callback)");
  if (me->m_connected)
    {
    me->m_connected = false;
    MyEvents.SignalEvent("system.modem.down",NULL);
    }

  if (me->m_shutdown)
    {
    // A late callback for a Shutdown() of ours (observed 5 s after ppp_close):
    // StopPPP() has stopped this session on purpose, the state machine will
    // call Startup() when it wants PPP back. Do not race it with a reconnect.
    ESP_LOGI(TAG, "PPP shutdown requested - not reconnecting");
    return;
    }

  // Try to reconnect in 30 seconds. This is assuming the modem level
  // data channel is still open.
  ESP_LOGI(TAG, "Attempting PPP reconnecting in 30 seconds...");
  // Note: We are in tiT task context here so use ppp_connect not pppapi_connect.
  ppp_connect(pcb, 30);
  }

GsmPPPOS::GsmPPPOS(modem* modem)
  {
  m_modem = modem;
  m_mux = NULL;
  m_channel = -1;
  m_ppp = NULL;
  m_connected = false;
  m_connectcount = 0;
  m_lasterrcode = -1;
  m_shutdown = true;
  }

GsmPPPOS::~GsmPPPOS()
  {
  Shutdown(true);
  if (m_ppp)
    {
    pppapi_free(m_ppp);
    m_ppp = NULL;
    }
  }

void GsmPPPOS::IncomingData(uint8_t *data, size_t len)
  {
  if (m_shutdown || m_ppp == NULL) return;  // Skip if shutting down
  MyCommandApp.HexDump(TAG, "rx", (const char*)data, len);
  pppos_input_tcpip(m_ppp, (u8_t*)data, (int)len);
  }

void GsmPPPOS::Initialise(GsmMux* mux, int channel)
  {
  ESP_LOGI(TAG, "Initialising...");

  m_mux = mux;
  m_channel = channel;

  if (m_ppp == NULL)
    {
    m_ppp = pppapi_pppos_create(&m_ppp_netif,
              GsmPPPOS_OutputCallback, GsmPPPOS_StatusCallback, this);
    }
  if (m_ppp == NULL)
    {
    ESP_LOGE(TAG, "Error init pppos");
    return;
    }
  // Only claim the default route when WiFi isn't already the active default.
  // NetManager's PrioritiseAndIndicate() will reassert WiFi anyway, but
  // skipping the unconditional set_default here avoids a transient route
  // flap on PPP cycling. If WiFi isn't up, we still want PPP as default so
  // the first packets after handoff (V2 reconnect, NTP) go out over LTE.
  if (!MyNetManager.m_connected_wifi)
    {
    pppapi_set_default(m_ppp);
    }
  }

void GsmPPPOS::DeInitialise()
  {
  m_mux = NULL;
  }

void GsmPPPOS::Startup()
  {
  if (!m_mux || !m_ppp) return;

  // A previous session may still be alive or sitting in its reconnect holdoff
  // (NetLoss reached with PPP already down takes no StopPPP()): bring the pcb
  // back to PPP_PHASE_DEAD before reconfiguring it, ppp_connect() refuses
  // anything else.
  if (!m_shutdown) Shutdown(true);

  m_shutdown = false;  // Reset shutdown flag
  ppp_set_auth(m_ppp, PPPAUTHTYPE_PAP,
    MyConfig.GetParamValue("modem", "apn.user").c_str(),
    MyConfig.GetParamValue("modem", "apn.password").c_str());
  ppp_set_usepeerdns(m_ppp, 1); // Ask the peer for up to 2 DNS server addresses

  // LCP echo keepalive (RFC 1661 Section 5.8): periodically probe the PPP
  // peer so we detect carrier-side PDP/NAT timeouts proactively instead of
  // discovering them only when an app pushes traffic (typically minutes
  // after WiFi -> LTE handoff). Free Mobile FR (CG-NAT) silently expires
  // idle PDP sessions after 5-10 min, which produced the documented
  // 5.5 min outage on 2026-04-30 when WiFi dropped on garage exit.
  //
  //   network.ppp.lcp_echo_interval  seconds between echoes (0 = off, default 30)
  //   network.ppp.lcp_echo_fails     consecutive failures before link declared dead (default 4)
  //
  // Defaults => dead-link declared in 4 * 30 = 120 s, well under typical
  // carrier idle timeouts. Set interval = 0 to disable (reverts to upstream).
  int lcp_int  = MyConfig.GetParamValueInt("network", "ppp.lcp_echo_interval", 30);
  int lcp_fail = MyConfig.GetParamValueInt("network", "ppp.lcp_echo_fails", 4);
  if (lcp_int  < 0)   lcp_int  = 0;
  if (lcp_int  > 255) lcp_int  = 255;
  if (lcp_fail < 1)   lcp_fail = 1;
  if (lcp_fail > 255) lcp_fail = 255;
  m_ppp->settings.lcp_echo_interval = (u8_t)lcp_int;
  m_ppp->settings.lcp_echo_fails    = (u8_t)lcp_fail;
  ESP_LOGI(TAG, "PPP LCP echo: interval=%us fails=%u (0 disables)",
           m_ppp->settings.lcp_echo_interval, m_ppp->settings.lcp_echo_fails);

  err_t err = pppapi_connect(m_ppp, 0);
  if (err != ERR_OK)
    {
    // ERR_ALREADY / ERR_VAL: the pcb was not in PPP_PHASE_DEAD. The NetMode
    // supervision in the modem state machine will notice the missing session.
    ESP_LOGE(TAG, "pppapi_connect failed: %d", (int)err);
    }
  }

void GsmPPPOS::Shutdown(bool hard)
  {
  if (m_ppp)
    {
    u8_t nocarrier = (u8_t)hard;
    if (hard)
      {
      ESP_LOGI(TAG, "Shutting down (hard)...");
      }
    else
      {
      ESP_LOGI(TAG, "Shutting down (soft)...");
      }
    m_shutdown = true;  // Prevent new data from being processed
    m_connected = false;
    MyEvents.SignalEvent("system.modem.down",NULL);
    vTaskDelay(pdMS_TO_TICKS(100));  // Allow pending data to drain
    pppapi_close(m_ppp, nocarrier);
    ESP_LOGI(TAG, "PPP is shutdown");
    }
  else
    {
    ESP_LOGI(TAG, "Shutdown (direct)");
    m_shutdown = true;
    m_connected = false;
    }
  }

const char* GsmPPPOS::ErrCodeName(int errcode)
  {
  switch (errcode)
    {
    case PPPERR_NONE:        return "None";
    case PPPERR_PARAM:       return "Invalid Parameter";
    case PPPERR_OPEN:        return "Unable to Open PPP";
    case PPPERR_DEVICE:      return "Invalid I/O Device";
    case PPPERR_ALLOC:       return "Unable to Allocate Resources";
    case PPPERR_USER:        return "User Interrupt";
    case PPPERR_CONNECT:     return "Connection Lost";
    case PPPERR_AUTHFAIL:    return "Authentication Failed";
    case PPPERR_PROTOCOL:    return "Failed to meet Protocol";
    case PPPERR_PEERDEAD:    return "Peer Dead";
    case PPPERR_IDLETIMEOUT: return "Idle Timeout";
    case PPPERR_CONNECTTIME: return "Max Connect Time Reached";
    case PPPERR_LOOPBACK:    return "Loopback Detected";
    default:                 return "Undefined";
    };
  }
