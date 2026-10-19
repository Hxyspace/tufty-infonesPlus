#include "ble.hpp"
#include "gamepad.hpp"
#include "bluetooth_manager.hpp"
#include "ble_gamepad.hpp"
#include "ble_tgp.hpp"
#include "tgp_legacy.hpp"
#include "power.hpp"
#include "usb.hpp"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ble {
namespace {
State state=State::Off;
bool initialized=false,working=false;
bool tracing=false;
bool remote_close=false,profile_failure_pending=false;
bluetooth::Manager manager;
gamepad::Receiver receiver;
bool tgp_profile=false;
uint8_t last_error=0;
uint32_t state_at=0,max_poll_gap=0,last_poll=0,max_poll_work=0,restarts=0;
uint32_t scan_reports=0,scan_candidates=0;
const char *restart_reason=nullptr;
hci_con_handle_t handle=HCI_CON_HANDLE_INVALID;
bd_addr_t peer{},skipped_peer{};
bd_addr_type_t peer_type=BD_ADDR_TYPE_LE_PUBLIC,skipped_type=BD_ADDR_TYPE_UNKNOWN;
uint32_t skipped_at=0;
btstack_packet_callback_registration_t hci_registration{},sm_registration{};
uint32_t now() {return to_ms_since_boot(get_absolute_time());}
const char *name() {
    switch(state) {
        case State::Off:return "OFF"; case State::Starting:return "STARTING";
        case State::Scanning:return "SCANNING"; case State::Connecting:return "CONNECTING";
        case State::Waiting:return "WAITING";
        case State::Discovering:return "DISCOVERING"; case State::Ready:return "READY";
        case State::Disconnecting:return "DISCONNECTING"; case State::Backoff:return "RETRY";
        case State::Recovering:return "RECOVERING";
        case State::Error:return "ERROR";
    }
    return "ERROR";
}
void phase(State value) {
    state=value; state_at=now();
    profile_failure_pending=false;
    if(value!=State::Ready) remote_close=false;
    if(value!=State::Ready && value!=State::Discovering) {ble_gamepad::reset(); ble_tgp::reset();}
}
void scan() {
    receiver.release();
    scan_reports=scan_candidates=0;
    gap_set_scan_parameters(1,96,48); // Active scan also receives scan responses.
    if(!manager.begin_scan(now())) {phase(State::Waiting); return;}
    phase(State::Scanning); gap_start_scan();
    std::printf("BLE: SCANNING HID\n");
}
void retry() {
    receiver.release(); manager.retry();
    phase(State::Backoff);
}
void recover(const char *reason) {
    std::printf("BLE: RECOVER reason=%s state=%s age_ms=%lu; restarting controller\n",reason,name(),
        static_cast<unsigned long>(now()-state_at));
    receiver.release(); ++restarts; restart_reason=nullptr;
    // Keep the stack/handlers alive. BTstack's halt watchdog closes a stalled
    // transport; wait for OFF before issuing ON, without reinitializing GATT.
    phase(State::Recovering);
    gap_stop_scan();
    hci_power_control(HCI_POWER_OFF);
}
void disconnect() {
    // gap_disconnect/cancel may deliver completion synchronously. Publish the
    // waiting state first so their callback's retry() remains authoritative.
    receiver.release(); phase(State::Disconnecting);
    if(handle!=HCI_CON_HANDLE_INVALID) gap_disconnect(handle);
    else gap_connect_cancel();
}
void reject(uint8_t reason) {
    // A failed pairing also fails the pending HIDS query. Both callbacks may
    // arrive for one failure; do not restart an in-progress disconnect/retry.
    if(state!=State::Discovering && state!=State::Ready) return;
    receiver.release(); last_error=reason;
    std::memcpy(skipped_peer,peer,sizeof(peer)); skipped_type=peer_type; skipped_at=now();
    std::printf("BLE: %s FAILED status=0x%02x\n",tgp_profile?"TGP":"HID",reason);
    if(handle!=HCI_CON_HANDLE_INVALID) disconnect();
    else retry();
}
void profile_event(ble_profile::Event event,uint8_t value) {
    switch(event) {
        case ble_profile::Event::Ready:
            if(state!=State::Discovering) return;
            phase(State::Ready); last_error=0;
            if(tgp_profile) std::printf("BLE: READY peer=%s protocol=TGP\n",bd_addr_to_str(peer));
            else std::printf("BLE: READY peer=%s report=%u\n",bd_addr_to_str(peer),value);
            break;
        case ble_profile::Event::Failed:
            if(state!=State::Discovering && state!=State::Ready) return;
            // HIDS maps GATT errors, including a lost link, to 0x1f. Finish
            // dispatching HCI first: disconnection clears this pending failure
            // and retries without the 30-second profile failure cooldown.
            if(value==ERROR_CODE_UNSPECIFIED_ERROR) {receiver.release(); profile_failure_pending=true;}
            else reject(value);
            break;
        case ble_profile::Event::RemoteClose:
            remote_close=true; std::printf("BLE: REMOTE CLOSE; disconnecting gamepad\n"); break;
        case ble_profile::Event::HeartbeatTimeout:
            if(!tgp_profile || state!=State::Ready) return;
            last_error=ERROR_CODE_CONNECTION_TIMEOUT;
            std::printf("BLE: TGP HEARTBEAT TIMEOUT; disconnecting and rescanning\n");
            disconnect(); break;
        case ble_profile::Event::Activity: power::activity(); break;
    }
}
void event(uint8_t packet_type,uint16_t,uint8_t *packet,uint16_t size) {
    if(packet_type!=HCI_EVENT_PACKET || size<2) return;
    switch(packet[0]) {
        case BTSTACK_EVENT_STATE:
            if(size<3) return;
            working=btstack_event_state_get_state(packet)==HCI_STATE_WORKING;
            if(!working) receiver.release();
            if(btstack_event_state_get_state(packet)==HCI_STATE_OFF) {
                handle=HCI_CON_HANDLE_INVALID; ble_gamepad::reset(); ble_tgp::reset();
            }
            if(working && manager.configuration().enabled && state==State::Starting) scan();
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            if(state!=State::Scanning || size<12 || size<12+gap_event_advertising_report_get_data_length(packet)) return;
            ++scan_reports;
            uint8_t kind=gap_event_advertising_report_get_advertising_event_type(packet);
            if(kind!=0 && kind!=4) return; // Connectable ADV_IND or scan response.
            const uint8_t *data=gap_event_advertising_report_get_data(packet);
            uint8_t bytes=gap_event_advertising_report_get_data_length(packet);
            bd_addr_t address;
            gap_event_advertising_report_get_address(packet,address);
            bd_addr_type_t type=static_cast<bd_addr_type_t>(gap_event_advertising_report_get_address_type(packet));
            if(type!=BD_ADDR_TYPE_LE_PUBLIC && type!=BD_ADDR_TYPE_LE_RANDOM) return;
            bluetooth::Peer key; std::memcpy(key.address,address,6); key.random=type==BD_ADDR_TYPE_LE_RANDOM;
            Candidate item; item.peer=key; item.seen_at=now();
            item.hid=ad_data_contains_uuid16(bytes,data,manager.configuration().scan_uuid);
            item.tgp=ad_data_contains_uuid128(bytes,data,tgp_legacy::ServiceUuid);
            item.rssi=gap_event_advertising_report_get_rssi(packet);
            bool complete_name=false;
            ad_context_t context;
            for(ad_iterator_init(&context,bytes,data);ad_iterator_has_more(&context);ad_iterator_next(&context)) {
                uint8_t ad_type=ad_iterator_get_data_type(&context);
                if(ad_type==BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
                    tgp_legacy::identity(ad_iterator_get_data(&context),ad_iterator_get_data_len(&context),item.controller_id,item.tgp_version);
                }
                if(ad_type!=BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME && ad_type!=BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) continue;
                if(ad_type==BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME && item.name[0]) continue;
                size_t n=std::min<size_t>(ad_iterator_get_data_len(&context),sizeof(item.name)-1);
                std::memcpy(item.name,ad_iterator_get_data(&context),n); item.name[n]=0;
                complete_name=ad_type==BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
            }
            const auto *cached=manager.observe(item,complete_name);
            if(cached) item=*cached;
            bool tgp=item.tgp && item.controller_id==manager.configuration().controller_id && tgp_legacy::supported(item.tgp_version);
            // A known TGP profile always uses ID filtering, including explicit
            // selections/whitelists. HID controllers have no TGP identity.
            if(item.tgp && !tgp && !item.hid) return;
            if(!manager.accepts(key,item.hid || tgp,now())) return;
            tgp_profile=tgp && !item.hid;
            if(type==skipped_type && !std::memcmp(address,skipped_peer,6) && now()-skipped_at<30000) return;
            ++scan_candidates;
            gap_stop_scan(); std::memcpy(peer,address,6); peer_type=type;
            std::printf("BLE: CONNECTING peer=%s type=%s\n",bd_addr_to_str(peer),type==BD_ADDR_TYPE_LE_RANDOM?"random":"public");
            phase(State::Connecting);
            uint8_t status=gap_connect(peer,peer_type);
            if(status) {
                last_error=status;
                std::printf("BLE: CONNECT REQUEST FAILED status=0x%02x\n",status);
                restart_reason="connection request failed";
                retry();
            }
            break;
        }
        case HCI_EVENT_META_GAP: {
            if(size<4 || packet[2]!=GAP_SUBEVENT_LE_CONNECTION_COMPLETE) return;
            uint8_t status=gap_subevent_le_connection_complete_get_status(packet);
            if(status) {
                last_error=status;
                if(state==State::Connecting || (state==State::Disconnecting && handle==HCI_CON_HANDLE_INVALID)) {
                    bool cancelled=state==State::Disconnecting && status==ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
                    std::printf("BLE: CONNECT %s status=0x%02x\n",cancelled?"CANCELLED":"FAILED",status);
                    if(!cancelled && !restart_reason) restart_reason="connection failed";
                    retry();
                }
                return;
            }
            if(size<12) return;
            handle=gap_subevent_le_connection_complete_get_connection_handle(packet);
            if(state==State::Recovering) {gap_disconnect(handle); return;}
            if(state!=State::Connecting || !manager.configuration().enabled) {disconnect(); return;}
            phase(State::Discovering);
            if(tgp_profile) ble_tgp::connect(handle,manager.configuration().controller_id);
            else {status=ble_gamepad::connect(handle); if(status) reject(status);}
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if(size<6 || hci_event_disconnection_complete_get_connection_handle(packet)!=handle) return;
            last_error=hci_event_disconnection_complete_get_reason(packet);
            std::printf("BLE: DISCONNECTED reason=0x%02x\n",last_error);
            handle=HCI_CON_HANDLE_INVALID;
            if(state!=State::Recovering) retry();
            break;
        case SM_EVENT_JUST_WORKS_REQUEST:
            if(size>=4 && sm_event_just_works_request_get_handle(packet)==handle)
                sm_just_works_confirm(handle);
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            if(size<13 || sm_event_pairing_complete_get_handle(packet)!=handle) return;
            uint8_t status=sm_event_pairing_complete_get_status(packet);
            uint8_t reason=sm_event_pairing_complete_get_reason(packet);
            if(status || tracing)
                std::printf("BLE: PAIRING %s status=0x%02x reason=0x%02x\n",status?"FAILED":"OK",status,reason);
            if(status) reject(status);
            break;
        }
        default:break;
    }
}
void report() {
    std::printf("BLE STATUS: state=%s buttons=0x%02x home=%u reports=%lu rejected=%lu poll_gap_ms=%lu poll_work_ms=%lu restarts=%lu mode=%s whitelist=%u\n",
        name(),receiver.nes(),receiver.home,
        static_cast<unsigned long>(receiver.received),static_cast<unsigned long>(receiver.rejected),
        static_cast<unsigned long>(max_poll_gap),static_cast<unsigned long>(max_poll_work),
        static_cast<unsigned long>(restarts),manager.configuration().policy.mode==bluetooth::Mode::WhitelistOnly?"whitelist":"auto",
        manager.configuration().policy.count);
    if(tgp_profile && handle!=HCI_CON_HANDLE_INVALID) {
        std::printf("BLE PEER: address=%s type=%s protocol=TGP id=%u version=%u\n",bd_addr_to_str(peer),
            peer_type==BD_ADDR_TYPE_LE_RANDOM?"random":"public",manager.configuration().controller_id,ble_tgp::version());
    } else {
        std::printf("BLE PEER: address=%s type=%s service=%u report=%u scan_uuid=0x%04X trace=%u\n",
            handle==HCI_CON_HANDLE_INVALID?"none":bd_addr_to_str(peer),
            peer_type==BD_ADDR_TYPE_LE_RANDOM?"random":"public",ble_gamepad::service(),ble_gamepad::report(),manager.configuration().scan_uuid,tracing);
    }
    std::printf("BLE SCAN: reports=%lu candidates=%lu\n",static_cast<unsigned long>(scan_reports),
        static_cast<unsigned long>(scan_candidates));
}
}
uint8_t buttons() {return receiver.nes();}
bool home() {return receiver.home;}
gamepad::Receiver &input_receiver() {return receiver;}
Status status() {
    bluetooth::Peer key; std::memcpy(key.address,peer,6); key.random=peer_type==BD_ADDR_TYPE_LE_RANDOM;
    return {state,manager.configuration().enabled,usb::disk_mode(),manager.discovering(),key,last_error};
}
void configure(const bluetooth::Configuration &config,bool reconnect) {
    manager.configure(config,reconnect);
    if(reconnect) receiver.release();
}
bool discover(uint32_t duration_ms) {
    return !usb::disk_mode() && manager.discover(duration_ms);
}
void stop_discovery() {manager.stop_discovery();}
size_t candidates(Candidate *out,size_t capacity) {return manager.candidates(out,capacity);}
bool connect(const bluetooth::Peer &key) {
    if(usb::disk_mode() || !manager.connect(key)) return false;
    receiver.release(); return true;
}
void init() {
    if(initialized || !manager.configuration().enabled || usb::disk_mode()) return;
    phase(State::Starting);
    int result=cyw43_arch_init();
    if(result) {phase(State::Error); std::printf("BLE: INIT FAILED rc=%d\n",result); return;}
    initialized=true;
    l2cap_init(); sm_init(); gatt_client_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // BTstack defaults to SC-only when ENABLE_LE_SECURE_CONNECTIONS is built.
    // Setting AuthReq=0 alone does not disable that policy. Match the XTARK
    // receiver's legacy Just Works pairing, without requiring bonding or MITM.
    sm_set_secure_connections_only_mode(false);
    sm_set_authentication_requirements(0); // Temporary pairing, without bonding.
    ble_gamepad::init(receiver,profile_event);
    ble_tgp::init(receiver,profile_event);
    hci_registration.callback=event; hci_add_event_handler(&hci_registration);
    sm_registration.callback=event; sm_add_event_handler(&sm_registration);
    gap_set_connection_parameters(48,48,6,12,0,100,0,0);
    last_poll=now(); phase(State::Starting);
    hci_power_control(HCI_POWER_ON);
}
void poll() {
    if(usb::disk_mode()) return;
    if(manager.take_request()) {
        receiver.release(); skipped_type=BD_ADDR_TYPE_UNKNOWN;
        if(initialized) {
            if(state==State::Scanning) gap_stop_scan();
            if(state==State::Disconnecting || state==State::Recovering || restart_reason) {} // Apply after completion.
            else if(state==State::Connecting || handle!=HCI_CON_HANDLE_INVALID) disconnect();
            else if(manager.configuration().enabled) {
                if(working) scan(); else {phase(State::Starting); hci_power_control(HCI_POWER_ON);}
            } else {phase(State::Off); hci_power_control(HCI_POWER_OFF);}
        } else if(manager.configuration().enabled) init();
    }
    if(!initialized) return;
    uint32_t at=now();
    if(at-last_poll>max_poll_gap) max_poll_gap=at-last_poll;
    last_poll=at;
    cyw43_arch_poll();
    if(profile_failure_pending) {
        profile_failure_pending=false;
        reject(ERROR_CODE_UNSPECIFIED_ERROR);
    }
    if(tgp_profile && (state==State::Discovering || state==State::Ready)) ble_tgp::poll(now());
    if(manager.finish_discovery(now())) {
        if(state==State::Scanning) {gap_stop_scan(); phase(State::Waiting);}
        std::printf("BLE: DISCOVERY COMPLETE devices=%u\n",unsigned(manager.count()));
    }
    // Finish HIDS callbacks before terminating the link, and never persistently
    // disable the radio for an Android app-close message.
    if(remote_close && state==State::Ready) disconnect();
    if(state==State::Ready && receiver.poll(now()) && receiver.contributing()) {
        power::activity();
    }
    uint32_t work=now()-at;
    if(work>max_poll_work) max_poll_work=work;
    if(work>=1000) std::printf("BLE: SLOW POLL work_ms=%lu\n",static_cast<unsigned long>(work));
    uint32_t elapsed=now()-state_at;
    if(state==State::Backoff && restart_reason) {
        // Cancellation completion only confirms the initiating procedure ended.
        // After a failed/timed-out attempt, reset the controller before scanning
        // again, matching the off/on recovery without cycling a healthy link.
        // Run outside the event callback so BTstack can finish freeing clients.
        recover(restart_reason);
    } else if(state==State::Recovering && hci_get_state()==HCI_STATE_OFF) {
        working=false; handle=HCI_CON_HANDLE_INVALID; retry();
    } else if(state==State::Backoff && (!manager.configuration().enabled || elapsed>=1000)) {
        if(manager.configuration().enabled) {
            if(working) scan();
            else {phase(State::Starting); hci_power_control(HCI_POWER_ON);}
        }
        else if(!manager.configuration().enabled) {phase(State::Off); hci_power_control(HCI_POWER_OFF);}
    } else if(state==State::Connecting && elapsed>=10000) {
        last_error=ERROR_CODE_CONNECTION_TIMEOUT;
        std::printf("BLE: CONNECT TIMEOUT; cancelling before controller restart\n");
        restart_reason="connection timeout";
        disconnect();
    } else if(state==State::Discovering && elapsed>=10000) reject(ERROR_CODE_CONNECTION_TIMEOUT);
    else if((state==State::Starting || state==State::Disconnecting) && elapsed>=10000) {
        recover(restart_reason?restart_reason:"state timeout");
    }
}
void shutdown() {
    receiver.release(); manager.reset();
    if(initialized) {
        if(state==State::Scanning) gap_stop_scan();
        if(state==State::Connecting) gap_connect_cancel();
        hci_power_control(HCI_POWER_OFF);
        cyw43_arch_deinit(); initialized=false;
    }
    working=false; restart_reason=nullptr; phase(State::Off);
}
bool command(const char *line) {
    if(!std::strcmp(line,"ble status")) report();
    else if(!std::strcmp(line,"ble trace on") || !std::strcmp(line,"ble trace off")) {
        tracing=!std::strcmp(line,"ble trace on"); ble_gamepad::trace(tracing);
        std::printf("BLE: TRACE %s\n",tracing?"ON":"OFF");
    }
    else if(!std::strncmp(line,"ble",3)) {
        std::printf("Unknown BLE command; type help\n");
    } else return false;
    return true;
}
}
