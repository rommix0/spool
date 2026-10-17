#pragma once
// Resource and command ids shared by sound_logger.rc and the window code.

#define IDR_ACCEL 101

// Commands (menus and accelerators).
#define IDM_MARKER_NOW 201
#define IDM_MARKER_5 202
#define IDM_MARKER_10 203
#define IDM_MARKER_30 204
#define IDM_SPLIT 205
#define IDM_ADD_FOLDER 206

// Child controls of the main window.
#define IDC_RECORD 301
#define IDC_STOP 302
#define IDC_CLOCK 303
#define IDC_DEVICE 304
#define IDC_MARKER 305
#define IDC_SPLIT 306
#define IDC_SETTINGS 307
#define IDC_LEVELS_LABEL 308
#define IDC_METERS 309
#define IDC_BUFFER 310
#define IDC_DESTS_LABEL 311
#define IDC_ACK 312
#define IDC_DESTS 313
#define IDC_EVENTS_LABEL 314
#define IDC_EVENTS 315

// Dialogs.
#define IDD_SETTINGS 401
#define IDD_DEST 402

// Settings dialog controls.
#define IDC_S_DEVICE 1001
#define IDC_S_MODE 1002
#define IDC_S_RATE_LABEL 1003
#define IDC_S_RATE 1004
#define IDC_S_RATE_NOTE 1005
#define IDC_S_RING 1006
#define IDC_S_DESTS 1007
#define IDC_S_DEST_ADD 1008
#define IDC_S_DEST_EDIT 1009
#define IDC_S_DEST_REMOVE 1010
#define IDC_S_SESSION 1011
#define IDC_S_FLUSH 1012
#define IDC_S_LOW_WATER 1013
#define IDC_S_STALL 1014
#define IDC_S_AUTOSTART 1015
#define IDC_S_HOTKEY 1016
#define IDC_S_RATE_NOTE_LABEL 1017

// Destination editor controls.
#define IDC_D_NAME 1101
#define IDC_D_FORMAT 1102
#define IDC_D_DEPTH_LABEL 1103
#define IDC_D_DEPTH 1104
#define IDC_D_BITRATE_LABEL 1105
#define IDC_D_BITRATE 1106
#define IDC_D_CHANNELS 1107
#define IDC_D_FOLDERS 1108
#define IDC_D_FOLDER_ADD 1109
#define IDC_D_FOLDER_REMOVE 1110
#define IDC_D_FOLDER_UP 1111
#define IDC_D_FOLDER_DOWN 1112
#define IDC_D_ROTATION 1113
#define IDC_D_TEMPLATE 1114
#define IDC_D_PREVIEW 1115
