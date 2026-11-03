#include "payto.h"

#include <furi.h>
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
#include <nfc/protocols/type_4_tag/type_4_tag.h>
#include <nfc/protocols/type_4_tag/type_4_tag_poller.h>
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
	Type4TagData* emulation;
	PaytoWallet wallets[PAYTO_MAX_WALLETS];
	size_t wallet_count;
	PaytoWallet pending;
	char pending_name[PAYTO_MAX_NAME_LEN + 1];
	char manual_entry[PAYTO_MAX_URI_LEN];
} PaytoApp;

static void payto_menu_callback(void* context, uint32_t index);
static void payto_show_pending_actions(PaytoApp* app);

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
	char data[PAYTO_MAX_WALLETS * (PAYTO_MAX_URI_LEN + PAYTO_MAX_NAME_LEN + 2)] = {0};
	size_t length = storage_file_read(file, data, sizeof(data) - 1);
	storage_file_close(file);
	storage_file_free(file);
	char* line = strtok(data, "\n");
	while(line && app->wallet_count < PAYTO_MAX_WALLETS) {
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
		line = strtok(NULL, "\n");
	}
}

static void payto_rebuild_menu(PaytoApp* app) {
	submenu_reset(app->menu);
	submenu_set_header(app->menu, "PayTo Wallets");
	submenu_add_item(app->menu, "Scan NFC", PAYTO_MENU_SCAN, payto_menu_callback, app);
	submenu_add_item(app->menu, "Enter manually", PAYTO_MENU_MANUAL, payto_menu_callback, app);
	for(size_t i = 0; i < app->wallet_count; i++) {
		char label[PAYTO_SHORT_NAME_LEN + PAYTO_MAX_NAME_LEN + 4];
		snprintf(label, sizeof(label), "%s%s%s", app->wallets[i].short_name,
			app->wallets[i].name[0] ? " (" : "", app->wallets[i].name);
		if(app->wallets[i].name[0]) strncat(label, ")", sizeof(label) - strlen(label) - 1);
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

static NfcCommand payto_poller_callback(NfcGenericEvent event, void* context) {
	PaytoApp* app = context;
	Type4TagPollerEvent* tag_event = event.event_data;
	if(tag_event->type == Type4TagPollerEventTypeReadSuccess) {
		const Type4TagData* data = nfc_poller_get_data(app->poller);
		if(simple_array_get_count(data->ndef_data) > 0 &&
			payto_parse_ndef(simple_array_cget_data(data->ndef_data), simple_array_get_count(data->ndef_data), &app->pending)) {
			view_dispatcher_send_custom_event(app->dispatcher, PAYTO_EVENT_SCANNED);
		}
		return NfcCommandStop;
	}
	return tag_event->type == Type4TagPollerEventTypeReadFailed ? NfcCommandReset : NfcCommandContinue;
}

static void payto_scanner_callback(NfcScannerEvent event, void* context) {
	PaytoApp* app = context;
	for(size_t i = 0; i < event.data.protocol_num; i++) {
		if(event.data.protocols[i] == NfcProtocolType4Tag) {
			nfc_scanner_stop(app->scanner);
			if(app->poller) nfc_poller_free(app->poller);
			app->poller = NULL;
			app->poller = nfc_poller_alloc(app->nfc, NfcProtocolType4Tag);
			nfc_poller_start(app->poller, payto_poller_callback, app);
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
		snprintf(uri, sizeof(uri), "payto://%s", entry);
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
		if(app->emulation) type_4_tag_free(app->emulation);
		app->emulation = type_4_tag_alloc();
		const size_t uri_length = strlen(wallet->uri);
		const size_t ndef_length = uri_length + 5;
		simple_array_init(app->emulation->ndef_data, ndef_length);
		uint8_t* ndef = simple_array_get_data(app->emulation->ndef_data);
		ndef[0] = 0xD1;
		ndef[1] = 0x01;
		ndef[2] = uri_length + 1;
		ndef[3] = 'U';
		ndef[4] = 0;
		memcpy(&ndef[5], wallet->uri, uri_length);
		app->listener = nfc_listener_alloc(app->nfc, NfcProtocolType4Tag, app->emulation);
		nfc_listener_start(app->listener, payto_listener_callback, app);
		submenu_set_header(app->menu, "Streaming NFC");
	}
}

static bool payto_custom_event(void* context, uint32_t event) {
	PaytoApp* app = context;
	if(event == PAYTO_EVENT_SCANNED) {
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
	PaytoApp app = {0};
	app.gui = furi_record_open(RECORD_GUI);
	app.storage = furi_record_open(RECORD_STORAGE);
	app.nfc = nfc_alloc();
	app.dispatcher = view_dispatcher_alloc();
	view_dispatcher_set_event_callback_context(app.dispatcher, &app);
	view_dispatcher_set_custom_event_callback(app.dispatcher, payto_custom_event);
	view_dispatcher_attach_to_gui(app.dispatcher, app.gui, ViewDispatcherTypeFullscreen);
	app.menu = submenu_alloc();
	app.text_input = text_input_alloc();
	view_set_previous_callback(submenu_get_view(app.menu), payto_exit);
	view_dispatcher_add_view(app.dispatcher, PaytoViewMenu, submenu_get_view(app.menu));
	view_dispatcher_add_view(app.dispatcher, PaytoViewName, text_input_get_view(app.text_input));
	payto_load_wallets(&app);
	payto_rebuild_menu(&app);
	view_dispatcher_switch_to_view(app.dispatcher, PaytoViewMenu);
	view_dispatcher_run(app.dispatcher);
	payto_stop_nfc(&app);
	if(app.listener) {
		nfc_listener_stop(app.listener);
		nfc_listener_free(app.listener);
	}
	if(app.emulation) type_4_tag_free(app.emulation);
	nfc_free(app.nfc);
	view_dispatcher_remove_view(app.dispatcher, PaytoViewMenu);
	view_dispatcher_remove_view(app.dispatcher, PaytoViewName);
	submenu_free(app.menu);
	text_input_free(app.text_input);
	view_dispatcher_free(app.dispatcher);
	furi_record_close(RECORD_STORAGE);
	furi_record_close(RECORD_GUI);
	return 0;
}
