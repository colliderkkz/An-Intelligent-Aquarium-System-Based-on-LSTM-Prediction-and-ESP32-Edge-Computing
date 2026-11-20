/** Active BLE discovery and GATT client for the selected aquarium lamp. */
#include "actuators/netlea_transport.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "NETLEA_BLE";
enum request { WAIT, SEARCH, CONNECT };
enum link_state { OFFLINE, DISCOVERING, UNSUPPORTED, FAILED, READY };
/* Requests and snapshots cross the MQTT, console and NimBLE tasks.
 * GAP procedures and discovery handles are owned by the NimBLE event queue. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static enum request wanted;
static unsigned generation;
static bool has_target;
static uint8_t target[6];
static netlea_scan_item_t found[NETLEA_SCAN_MAX];
static int count;
static struct ble_npl_event drive_event;
static atomic_bool synced;
static atomic_int state;
static unsigned applied;
static enum request mode;
static uint8_t selected[6], own_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svc_start, svc_end, notify_handle, notify_end, write_handle, cccd;
static int64_t next_scan;
static netlea_rx_cb_t rx;

static bool uuid_matches(const ble_uuid_t *uuid, uint16_t short_id) {
    ble_uuid128_t full = BLE_UUID128_INIT(0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,
                                         0,0x10,0,0,0,0,0,0);
    full.value[12] = short_id;
    full.value[13] = short_id >> 8;
    return ble_uuid_u16(uuid) == short_id || ble_uuid_cmp(uuid, &full.u) == 0;
}

static int subscribed(uint16_t h, const struct ble_gatt_error *e,
                      struct ble_gatt_attr *a, void *arg) {
    if (h == conn && state == DISCOVERING) state = e->status ? FAILED : READY;
    return 0;
}

static int descriptor(uint16_t h, const struct ble_gatt_error *e, uint16_t value,
                      const struct ble_gatt_dsc *d, void *arg) {
    if (h != conn || state != DISCOVERING) return 0;
    if (!e->status && d && uuid_matches(&d->uuid.u, 0x2902)) cccd = d->handle;
    if (e->status == BLE_HS_EDONE && cccd) {
        uint8_t enable[] = {1, 0};
        if (ble_gattc_write_flat(h, cccd, enable, sizeof(enable), subscribed, NULL))
            state = FAILED;
    } else if (e->status) {
        state = e->status == BLE_HS_EDONE ? UNSUPPORTED : FAILED;
    }
    return 0;
}

static int characteristic(uint16_t h, const struct ble_gatt_error *e,
                          const struct ble_gatt_chr *c, void *arg) {
    if (h != conn || state != DISCOVERING) return 0;
    if (!e->status && c) {
        if (notify_handle && notify_end == svc_end && c->def_handle > notify_handle)
            notify_end = c->def_handle - 1;
        if (uuid_matches(&c->uuid.u, 0xff01) && (c->properties & BLE_GATT_CHR_PROP_NOTIFY))
            notify_handle = c->val_handle;
        if (uuid_matches(&c->uuid.u, 0xff02) && (c->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP))
            write_handle = c->val_handle;
    } else if (e->status == BLE_HS_EDONE) {
        /* CMD16 has 23 bytes of payload on ATT, plus the 3-byte ATT header. */
        if (!write_handle || !notify_handle || notify_end <= notify_handle || ble_att_mtu(h) < 26)
            state = UNSUPPORTED;
        else if (ble_gattc_disc_all_dscs(h, notify_handle, notify_end, descriptor, NULL)) state = FAILED;
    } else {
        state = FAILED;
    }
    return 0;
}

static int service(uint16_t h, const struct ble_gatt_error *e,
                   const struct ble_gatt_svc *s, void *arg) {
    if (h != conn || state != DISCOVERING) return 0;
    if (!e->status && s && uuid_matches(&s->uuid.u, 0xfffa)) {
        svc_start = s->start_handle;
        notify_end = svc_end = s->end_handle;
    } else if (e->status == BLE_HS_EDONE) {
        if (!svc_start) state = UNSUPPORTED;
        else if (ble_gattc_disc_all_chrs(h, svc_start, svc_end, characteristic, NULL)) state = FAILED;
    } else if (e->status) {
        state = FAILED;
    }
    return 0;
}

static void remember(const struct ble_gap_disc_desc *d, bool connectable) {
    struct ble_hs_adv_fields f = {0};
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data)) f.name = NULL;
    portENTER_CRITICAL(&lock);
    if (wanted == SEARCH && applied == generation) {
        int i = 0;
        while (i < count && memcmp(found[i].mac, d->addr.val, 6)) i++;
        if (i == count && connectable && count < NETLEA_SCAN_MAX) {
            memset(&found[count], 0, sizeof(found[count]));
            memcpy(found[count++].mac, d->addr.val, 6);
        }
        if (i < count) {
            found[i].rssi = d->rssi;
            if (f.name && f.name_len) {
                size_t n = f.name_len < sizeof(found[i].name) ? f.name_len : sizeof(found[i].name)-1;
                memcpy(found[i].name, f.name, n);
                found[i].name[n] = 0;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
}

static bool current(void) {
    portENTER_CRITICAL(&lock);
    bool result = generation == applied;
    portEXIT_CRITICAL(&lock);
    return result;
}

static int mtu_exchanged(uint16_t h, const struct ble_gatt_error *e, uint16_t mtu, void *arg) {
    if (h == conn && state == DISCOVERING && ble_gattc_disc_all_svcs(h, service, NULL))
        state = FAILED;
    return 0;
}

static int gap(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
    case BLE_GAP_EVENT_DISC: {
        bool can_connect = e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        if (mode == SEARCH) {
            if (can_connect || e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
                remember(&e->disc, can_connect);
        } else if (mode == CONNECT && can_connect && current() &&
                   memcmp(selected, e->disc.addr.val, 6) == 0 && !ble_gap_disc_cancel()) {
            int rc = ble_gap_connect(own_type, &e->disc.addr, 10000, NULL, gap, NULL);
            if (rc) ESP_LOGW(TAG, "connect rc=%d", rc);
        }
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (mode == SEARCH) {
            portENTER_CRITICAL(&lock);
            if (generation == applied) {
                wanted = has_target ? CONNECT : WAIT;
                generation++;
            }
            portEXIT_CRITICAL(&lock);
            mode = WAIT;
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (!e->connect.status) {
            conn = e->connect.conn_handle;
            if (!current() || mode != CONNECT) {
                ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            svc_start = svc_end = notify_handle = notify_end = write_handle = cccd = 0;
            state = DISCOVERING;
            ble_att_set_preferred_mtu(500);
            if (ble_gattc_exchange_mtu(conn, mtu_exchanged, NULL)) state = FAILED;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        state = OFFLINE;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (e->notify_rx.conn_handle == conn && e->notify_rx.attr_handle == notify_handle && rx && state == READY) {
            uint8_t buf[64];
            uint16_t len;
            if (!ble_hs_mbuf_to_flat(e->notify_rx.om, buf, sizeof(buf), &len)) rx(buf, len);
        }
        break;
    }
    return 0;
}

static void drive(struct ble_npl_event *event) {
    if (!synced) return;
    if (!current()) {
        if (conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        if (ble_gap_conn_active()) { ble_gap_conn_cancel(); return; }
        if (ble_gap_disc_active() && ble_gap_disc_cancel()) return;
        portENTER_CRITICAL(&lock);
        applied = generation;
        mode = wanted;
        memcpy(selected, target, 6);
        portEXIT_CRITICAL(&lock);
        next_scan = 0;
    }
    if (mode == WAIT || conn != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_conn_active() || ble_gap_disc_active()) return;
    int64_t now = esp_timer_get_time();
    if (now < next_scan) return;
    next_scan = now + 1000000;
    struct ble_gap_disc_params p = {.itvl = 100, .window = 99};
    int rc = ble_gap_disc(own_type, mode == SEARCH ? 7000 : BLE_HS_FOREVER, &p, gap, NULL);
    if (rc) ESP_LOGW(TAG, "scan rc=%d", rc);
}

static void on_sync(void) {
    synced = ble_hs_id_infer_auto(0, &own_type) == 0;
}
static void on_reset(int reason) {
    synced = false;
    conn = BLE_HS_CONN_HANDLE_NONE;
    state = OFFLINE;
}
static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
esp_err_t netlea_transport_init(void) {
    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) return rc;
    ble_npl_event_init(&drive_event, drive, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
void netlea_transport_tick(void) {
    if (synced) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &drive_event);
}
bool netlea_transport_ready(void) { return state == READY && current(); }
const char *netlea_transport_name(void) {
    if (!current()) return "ble-scan";
    switch (state) {
    case READY: return "ble";
    case UNSUPPORTED: return "ble-unsupported";
    case FAILED: return "ble-gatt-error";
    case DISCOVERING: return "ble-discovering";
    default: return "ble-scan";
    }
}
void netlea_transport_force_sim(bool on) { (void)on; }
bool netlea_transport_is_sim(void) { return false; }
void netlea_transport_set_rx(netlea_rx_cb_t cb) { rx = cb; }

esp_err_t netlea_transport_write(const uint8_t *buf, size_t len) {
    if (!netlea_transport_ready()) return ESP_ERR_INVALID_STATE;
    return ble_gattc_write_no_rsp_flat(conn, write_handle, buf, len) ? ESP_FAIL : ESP_OK;
}
void netlea_transport_set_target(const uint8_t mac[6]) {
    if (!mac) return;
    portENTER_CRITICAL(&lock);
    memcpy(target, mac, 6);
    has_target = true;
    wanted = CONNECT;
    generation++;
    portEXIT_CRITICAL(&lock);
}
void netlea_transport_get_target(uint8_t mac[6]) {
    portENTER_CRITICAL(&lock);
    memcpy(mac, target, 6);
    portEXIT_CRITICAL(&lock);
}
void netlea_transport_scan_start(void) {
    portENTER_CRITICAL(&lock);
    if (wanted != SEARCH) {
        wanted = SEARCH;
        count = 0;
        generation++;
    }
    portEXIT_CRITICAL(&lock);
}
int netlea_transport_scan_items(netlea_scan_item_t *out, int max) {
    if (!out || max <= 0) return 0;
    portENTER_CRITICAL(&lock);
    int n = count < max ? count : max;
    memcpy(out, found, sizeof(*out) * n);
    portEXIT_CRITICAL(&lock);
    return n;
}
bool netlea_transport_scan_done(void) {
    portENTER_CRITICAL(&lock);
    bool done = wanted != SEARCH;
    portEXIT_CRITICAL(&lock);
    return done;
}
