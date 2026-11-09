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

/* the Change Setting dialog */
#define IDD_SETTING     950
#define IDC_S_LABEL     951
#define IDC_S_HELP      952
#define IDC_S_LIST      953
#define IDC_S_EDIT      954
#define IDC_S_RANGE     955
#define IDC_S_DEFAULT   956

#endif
