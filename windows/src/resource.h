/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * resource.h - the Windows app's resource and control ids
 */
#ifndef DMODEM_WIN_RESOURCE_H
#define DMODEM_WIN_RESOURCE_H

#define IDI_APP         1
#define IDR_MENU        100

#define IDM_LINE        200
#define IDM_START       201
#define IDM_EXIT        202
#define IDM_ABOUT       203

#define IDC_STATUS      300
#define IDC_TABS        301

/* the Modem page */
#define IDC_STATE       400
#define IDC_DETAIL      401
#define IDC_NUMBER      402
#define IDC_DIAL        403
#define IDC_ANSWER      404
#define IDC_HANGUP      405
#define IDC_RUN         406
#define IDC_SUMMARY     407
#define IDC_DEVICE      408
#define IDC_LINEBTN     409
#define IDC_CALLBOX     410
#define IDC_MODEMBOX    411
#define IDC_NUMLABEL    412

/* the Terminal page */
#define IDC_TERM        500
#define IDC_SETUP_CF    501
#define IDC_SETUP_CT    502
#define IDC_CLEAR       503
#define IDC_TERM_TYPELABEL 504
#define IDC_TERM_TYPE   505

/* the Remote page */
#define IDC_R_PROTO     1100
#define IDC_R_HOSTLABEL 1101
#define IDC_R_HOST      1102
#define IDC_R_PORTLABEL 1103
#define IDC_R_PORT      1104
#define IDC_R_USERLABEL 1105
#define IDC_R_USER      1106
#define IDC_R_GO        1107
#define IDC_R_TERM      1108
#define IDC_R_STATUS    1109
#define IDC_R_TYPELABEL 1110
#define IDC_R_TYPE      1111
#define IDC_R_LOGIN     1112

/* the Settings page */
#define IDC_GROUPS      600
#define IDC_SETTINGS    601
#define IDC_SETTING_HELP        602
#define IDC_RELOAD      603
#define IDC_DEFAULTS    604
#define IDC_INIT        605
#define IDC_KEEP        606
#define IDC_CHANGE      607

/* the Call Stats page */
#define IDC_STATS       700
#define IDC_REPORT      701
#define IDC_REFRESH     702

/* the Log page */
#define IDC_LOG         800

/* the Phone page */
#define IDC_V_STATUS    1000
#define IDC_V_CALLBOX   1001
#define IDC_V_NUMLABEL  1002
#define IDC_V_NUMBER    1003
#define IDC_V_CALL      1004
#define IDC_V_ANSWER    1005
#define IDC_V_MESSAGE   1006
#define IDC_V_PICKUP    1007
#define IDC_V_HANGUP    1008
#define IDC_V_MUTE      1009
#define IDC_V_LISTEN    1010
#define IDC_V_VMBOX     1011
#define IDC_V_VMON      1012
#define IDC_V_RINGSLABEL 1013
#define IDC_V_RINGS     1014
#define IDC_V_GREETING  1015
#define IDC_V_RECORD    1016
#define IDC_V_PLAYGREET 1017
#define IDC_V_CHOOSE    1018
#define IDC_V_CLEARGREET 1019
#define IDC_V_MSGBOX    1020
#define IDC_V_MESSAGES  1021
#define IDC_V_PLAY      1022
#define IDC_V_DELETE    1023
#define IDC_V_KEY       1030    /* 1030 to 1041: 1-9, *, 0, # */

/* the Line Settings dialog */
#define IDD_LINE        900
#define IDC_L_DMODEMD   901
#define IDC_L_TCP       902
#define IDC_L_COM       903
#define IDC_L_USER      904
#define IDC_L_PASSWORD  905
#define IDC_L_SERVER    906
#define IDC_L_LISTEN    907
#define IDC_L_T38       908
#define IDC_L_V152      909
#define IDC_L_V150      910
#define IDC_L_COUNTRY   911
#define IDC_L_USB       912
#define IDC_L_PATH      913
#define IDC_L_BROWSE    914
#define IDC_L_ADDRESS   915
#define IDC_L_PORT      916
#define IDC_L_INIT      917
#define IDC_L_ANONYMOUS 918

/* the Change Setting dialog */
#define IDD_SETTING     950
#define IDC_S_LABEL     951
#define IDC_S_HELP      952
#define IDC_S_LIST      953
#define IDC_S_EDIT      954
#define IDC_S_RANGE     955
#define IDC_S_DEFAULT   956

/* the Remote page's SSH questions: a heading, words, four answers */
#define IDD_ASK         960
#define IDC_A_HEAD      961
#define IDC_A_TEXT      962
#define IDC_A_Q0        963
#define IDC_A_E0        967
#define IDC_A_E0P       971

/* the Remote page's SSH Login dialog: the way, the key file, the agent */
#define IDD_LOGIN       980
#define IDC_K_HOW       981
#define IDC_K_KEY       982
#define IDC_K_BROWSE    983
#define IDC_K_IMPORT    984
#define IDC_K_AGENT     985
#define IDC_K_STATE     986

#endif
