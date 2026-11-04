#include "payto.h"

#include <furi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <nfc/nfc.h>
#include <nfc/nfc_listener.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_scanner.h>
#include <lib/nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <lib/nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <storage/storage.h>

#define PAYTO_STORAGE_PATH EXT_PATH("apps_data/flipper_payto/wallets.txt")
#define PAYTO_MAX_WALLETS 32
#define PAYTO_EVENT_SCANNED 1
#define PAYTO_MENU_SCAN 1
#define PAYTO_MENU_MANUAL 5
#define PAYTO_MENU_SAVE 2
#define PAYTO_MENU_NAME 3
#define PAYTO_MENU_DISCARD 4
#define PAYTO_MENU_WALLET 100
#define PAYTO_NDEF_OFFSET (4 * MF_ULTRALIGHT_PAGE_SIZE)
#define PAYTO_WALLET_FILE_BUFFER_SIZE (PAYTO_MAX_WALLETS * (PAYTO_MAX_URI_LEN + PAYTO_MAX_NAME_LEN + 2))
#define PAYTO_EVENT_START_POLLER 2

typedef enum {
	PaytoViewMenu,
	PaytoViewName,
} PaytoView;

typedef struct {
	Gui* gui;
	ViewDispatcher* dispatcher;
	Submenu* menu;
	TextInput* text_input;
	Storage* storage;
	Nfc* nfc;
	NfcScanner* scanner;
	NfcPoller* poller;
	NfcListener* listener;
	MfUltralightData* emulation;
	PaytoWallet wallets[PAYTO_MAX_WALLETS];
	size_t wallet_count;
	PaytoWallet pending;
	char pending_name[PAYTO_MAX_NAME_LEN + 1];
	char manual_entry[PAYTO_MAX_URI_LEN];
	char wallet_file_data[PAYTO_WALLET_FILE_BUFFER_SIZE];
	bool scanner_transition_pending;
} PaytoApp;

static void payto_menu_callback(void* context, uint32_t index);
static void payto_show_pending_actions(PaytoApp* app);
static NfcCommand payto_poller_callback(NfcGenericEvent event, void* context);

static bool payto_parse_ntag_ndef(const MfUltralightData* data, PaytoWallet* wallet) {
	const size_t byte_count = (size_t)data->pages_read * MF_ULTRALIGHT_PAGE_SIZE;
	const uint8_t* bytes = (const uint8_t*)data->page;
	for(size_t i = 4; i + 2 < byte_count; i++) {
		if(bytes[i] != 0x03) continue;
		size_t ndef_length = bytes[i + 1];
		size_t payload_offset = i + 2;
		if(ndef_length == 0xFF) {
			if(i + 4 > byte_count) return false;
			ndef_length = ((size_t)bytes[i + 2] << 8) | bytes[i + 3];
			payload_offset = i + 4;
		}
		if(payload_offset + ndef_length > byte_count) return false;
		return payto_parse_ndef(bytes + payload_offset, ndef_length, wallet);
	}
	return false;
}

static NfcCommand payto_listener_callback(NfcGenericEvent event, void* context) {
	UNUSED(event);
	UNUSED(context);
	return NfcCommandContinue;
}

static void payto_save_wallets(PaytoApp* app) {
	storage_common_mkdir(app->storage, EXT_PATH("apps_data"));
	storage_common_mkdir(app->storage, EXT_PATH("apps_data/flipper_payto"));
	File* file = storage_file_alloc(app->storage);
	if(storage_file_open(file, PAYTO_STORAGE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
		for(size_t i = 0; i < app->wallet_count; i++) {
			char line[PAYTO_MAX_URI_LEN + PAYTO_MAX_NAME_LEN + 4];
			int length = snprintf(line, sizeof(line), "%s\t%s\n", app->wallets[i].uri, app->wallets[i].name);
			storage_file_write(file, line, length);
		}
		storage_file_close(file);
	}
	storage_file_free(file);
}

static void payto_load_wallets(PaytoApp* app) {
	File* file = storage_file_alloc(app->storage);
	if(!storage_file_open(file, PAYTO_STORAGE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
		storage_file_free(file);
		return;
	}
	size_t length = storage_file_read(file, app->wallet_file_data, sizeof(app->wallet_file_data) - 1);
	app->wallet_file_data[length] = '\0';
	storage_file_close(file);
	storage_file_free(file);
	char* line = app->wallet_file_data;
	while(*line && app->wallet_count < PAYTO_MAX_WALLETS) {
		char* next_line = strchr(line, '\n');
		if(next_line) *next_line = '\0';
		char* separator = strchr(line, '\t');
		if(separator) {
			*separator++ = '\0';
			PaytoWallet* wallet = &app->wallets[app->wallet_count];
			if(payto_parse_uri(line, wallet)) {
				strncpy(wallet->name, separator, PAYTO_MAX_NAME_LEN);
				wallet->name[PAYTO_MAX_NAME_LEN] = '\0';
				app->wallet_count++;
			}
		}
		if(!next_line) break;
		line = next_line + 1;
	}
}

static void payto_rebuild_menu(PaytoApp* app) {
	submenu_reset(app->menu);
	submenu_set_header(app->menu, "PayTo Wallets");
	submenu_add_item(app->menu, "Scan NFC", PAYTO_MENU_SCAN, payto_menu_callback, app);
	submenu_add_item(app->menu, "Enter manually", PAYTO_MENU_MANUAL, payto_menu_callback, app);
	for(size_t i = 0; i < app->wallet_count; i++) {
		char label[PAYTO_SHORT_NAME_LEN + PAYTO_MAX_NAME_LEN + 4];
		snprintf(label, sizeof(label), "%s", app->wallets[i].short_name);
		if(app->wallets[i].name[0]) {
			size_t label_length = strlen(label);
			snprintf(label + label_length, sizeof(label) - label_length, " (%s)", app->wallets[i].name);
		}
		submenu_add_item(app->menu, label, PAYTO_MENU_WALLET + i, payto_menu_callback, app);
	}
}

static void payto_stop_nfc(PaytoApp* app) {
	if(app->scanner) {
		nfc_scanner_stop(app->scanner);
		nfc_scanner_free(app->scanner);
		app->scanner = NULL;
	}
	if(app->poller) {
		nfc_poller_stop(app->poller);
		nfc_poller_free(app->poller);
		app->poller = NULL;
	}
}

static void payto_start_poller(PaytoApp* app) {
	if(app->poller) {
		nfc_poller_stop(app->poller);
		nfc_poller_free(app->poller);
	}
	app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
	nfc_poller_start(app->poller, payto_poller_callback, app);
}

static NfcCommand payto_poller_callback(NfcGenericEvent event, void* context) {
	PaytoApp* app = context;
	MfUltralightPollerEvent* tag_event = event.event_data;
	if(tag_event->type == MfUltralightPollerEventTypeReadSuccess) {
		const MfUltralightData* data = nfc_poller_get_data(app->poller);
		if(payto_parse_ntag_ndef(data, &app->pending)) {
			view_dispatcher_send_custom_event(app->dispatcher, PAYTO_EVENT_SCANNED);
		}
		return NfcCommandStop;
	}
	return tag_event->type == MfUltralightPollerEventTypeReadFailed ? NfcCommandReset : NfcCommandContinue;
}

static void payto_scanner_callback(NfcScannerEvent event, void* context) {
	PaytoApp* app = context;
	for(size_t i = 0; i < event.data.protocol_num; i++) {
		if(event.data.protocols[i] == NfcProtocolMfUltralight) {
			if(!app->scanner_transition_pending) {
				app->scanner_transition_pending = true;
				view_dispatcher_send_custom_event(app->dispatcher, PAYTO_EVENT_START_POLLER);
			}
			return;
		}
	}
}

static void payto_start_scan(PaytoApp* app) {
	payto_stop_nfc(app);
	app->scanner = nfc_scanner_alloc(app->nfc);
	nfc_scanner_start(app->scanner, payto_scanner_callback, app);
	submenu_set_header(app->menu, "Hold near NFC");
}

static void payto_text_input_done(void* context) {
	PaytoApp* app = context;
	strncpy(app->pending.name, app->pending_name, PAYTO_MAX_NAME_LEN);
	app->pending.name[PAYTO_MAX_NAME_LEN] = '\0';
	if(app->wallet_count < PAYTO_MAX_WALLETS) app->wallets[app->wallet_count++] = app->pending;
	payto_save_wallets(app);
	payto_rebuild_menu(app);
	view_dispatcher_switch_to_view(app->dispatcher, PaytoViewMenu);
}

static void payto_manual_input_done(void* context) {
	PaytoApp* app = context;
	char uri[PAYTO_MAX_URI_LEN];
	const char* entry = app->manual_entry;
	if(strncasecmp(entry, "payto://", 8) == 0) {
		snprintf(uri, sizeof(uri), "%s", entry);
	} else {
		snprintf(uri, sizeof(uri), "payto://%.183s", entry);
	}
	if(payto_parse_uri(uri, &app->pending)) {
		payto_show_pending_actions(app);
	} else {
		submenu_set_header(app->menu, "Invalid PayTo URI");
		payto_rebuild_menu(app);
	}
	view_dispatcher_switch_to_view(app->dispatcher, PaytoViewMenu);
}

static void payto_save_pending(PaytoApp* app, bool ask_for_name) {
	if(ask_for_name) {
		memset(app->pending_name, 0, sizeof(app->pending_name));
		text_input_reset(app->text_input);
		text_input_set_header_text(app->text_input, "Wallet name (optional)");
		text_input_set_result_callback(app->text_input, payto_text_input_done, app, app->pending_name, sizeof(app->pending_name), true);
		view_dispatcher_switch_to_view(app->dispatcher, PaytoViewName);
	} else {
		app->pending.name[0] = '\0';
		if(app->wallet_count < PAYTO_MAX_WALLETS) app->wallets[app->wallet_count++] = app->pending;
		payto_save_wallets(app);
		payto_rebuild_menu(app);
	}
}

static void payto_menu_callback(void* context, uint32_t index) {
	PaytoApp* app = context;
	if(index == PAYTO_MENU_SCAN) payto_start_scan(app);
	else if(index == PAYTO_MENU_MANUAL) {
		memset(app->manual_entry, 0, sizeof(app->manual_entry));
		text_input_reset(app->text_input);
		text_input_set_header_text(app->text_input, "Enter currency/address");
		text_input_set_result_callback(
			app->text_input,
			payto_manual_input_done,
			app,
			app->manual_entry,
			sizeof(app->manual_entry),
			true);
		text_input_set_minimum_length(app->text_input, 3);
		view_dispatcher_switch_to_view(app->dispatcher, PaytoViewName);
	}
	else if(index == PAYTO_MENU_SAVE) payto_save_pending(app, false);
	else if(index == PAYTO_MENU_NAME) payto_save_pending(app, true);
	else if(index == PAYTO_MENU_DISCARD) payto_rebuild_menu(app);
	else if(index >= PAYTO_MENU_WALLET && index < PAYTO_MENU_WALLET + app->wallet_count) {
		PaytoWallet* wallet = &app->wallets[index - PAYTO_MENU_WALLET];
		const size_t uri_length = strlen(wallet->uri);
		const size_t ndef_length = uri_length + 5;
		const size_t emulation_size = (size_t)mf_ultralight_get_pages_total(MfUltralightTypeNTAG213) * MF_ULTRALIGHT_PAGE_SIZE;
		if(ndef_length > 255 || PAYTO_NDEF_OFFSET + 2 + ndef_length + 1 > emulation_size) {
			submenu_set_header(app->menu, "URI too long for NTAG213");
			return;
		}
		if(app->emulation) mf_ultralight_free(app->emulation);
		app->emulation = mf_ultralight_alloc();
		app->emulation->type = MfUltralightTypeNTAG213;
		app->emulation->pages_total = mf_ultralight_get_pages_total(app->emulation->type);
		uint8_t* ndef = (uint8_t*)app->emulation->page;
		ndef[PAYTO_NDEF_OFFSET] = 0x03;
		ndef[PAYTO_NDEF_OFFSET + 1] = (uint8_t)ndef_length;
		ndef[PAYTO_NDEF_OFFSET + 2] = 0xD1;
		ndef[PAYTO_NDEF_OFFSET + 3] = 0x01;
		ndef[PAYTO_NDEF_OFFSET + 4] = (uint8_t)(uri_length + 1);
		ndef[PAYTO_NDEF_OFFSET + 5] = 'U';
		ndef[PAYTO_NDEF_OFFSET + 6] = 0;
		memcpy(&ndef[PAYTO_NDEF_OFFSET + 7], wallet->uri, uri_length);
		ndef[PAYTO_NDEF_OFFSET + ndef_length + 2] = 0xFE;
		app->emulation->pages_read = app->emulation->pages_total;
		app->listener = nfc_listener_alloc(app->nfc, NfcProtocolMfUltralight, app->emulation);
		nfc_listener_start(app->listener, payto_listener_callback, app);
		submenu_set_header(app->menu, "Streaming NFC");
	}
}

static bool payto_custom_event(void* context, uint32_t event) {
	PaytoApp* app = context;
	if(event == PAYTO_EVENT_START_POLLER) {
		app->scanner_transition_pending = false;
		if(app->scanner) {
			nfc_scanner_stop(app->scanner);
			nfc_scanner_free(app->scanner);
			app->scanner = NULL;
		}
		payto_start_poller(app);
		return true;
	} else if(event == PAYTO_EVENT_SCANNED) {
		payto_stop_nfc(app);
		payto_show_pending_actions(app);
		return true;
	}
	return false;
}

static void payto_show_pending_actions(PaytoApp* app) {
	submenu_reset(app->menu);
	submenu_set_header(app->menu, app->pending.short_name);
	submenu_add_item(app->menu, "Save unnamed", PAYTO_MENU_SAVE, payto_menu_callback, app);
	submenu_add_item(app->menu, "Add name", PAYTO_MENU_NAME, payto_menu_callback, app);
	submenu_add_item(app->menu, "Discard", PAYTO_MENU_DISCARD, payto_menu_callback, app);
}

static uint32_t payto_exit(void* context) {
	PaytoApp* app = context;
	payto_stop_nfc(app);
	if(app->listener) nfc_listener_stop(app->listener);
	return VIEW_NONE;
}

int32_t flipper_payto_app(void* context) {
	UNUSED(context);
	PaytoApp* app = malloc(sizeof(PaytoApp));
	if(!app) return -1;
	memset(app, 0, sizeof(PaytoApp));
	app->gui = furi_record_open(RECORD_GUI);
	app->storage = furi_record_open(RECORD_STORAGE);
	app->nfc = nfc_alloc();
	app->dispatcher = view_dispatcher_alloc();
	view_dispatcher_set_event_callback_context(app->dispatcher, app);
	view_dispatcher_set_custom_event_callback(app->dispatcher, payto_custom_event);
	view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
	app->menu = submenu_alloc();
	app->text_input = text_input_alloc();
	view_set_previous_callback(submenu_get_view(app->menu), payto_exit);
	view_dispatcher_add_view(app->dispatcher, PaytoViewMenu, submenu_get_view(app->menu));
	view_dispatcher_add_view(app->dispatcher, PaytoViewName, text_input_get_view(app->text_input));
	payto_load_wallets(app);
	payto_rebuild_menu(app);
	view_dispatcher_switch_to_view(app->dispatcher, PaytoViewMenu);
	view_dispatcher_run(app->dispatcher);
	payto_stop_nfc(app);
	if(app->listener) {
		nfc_listener_stop(app->listener);
		nfc_listener_free(app->listener);
	}
	if(app->emulation) mf_ultralight_free(app->emulation);
	nfc_free(app->nfc);
	view_dispatcher_remove_view(app->dispatcher, PaytoViewMenu);
	view_dispatcher_remove_view(app->dispatcher, PaytoViewName);
	submenu_free(app->menu);
	text_input_free(app->text_input);
	view_dispatcher_free(app->dispatcher);
	furi_record_close(RECORD_STORAGE);
	furi_record_close(RECORD_GUI);
	free(app);
	return 0;
}
