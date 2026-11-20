/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * commctrl.h - the common controls of the Win32 layer (clang-retro MFC's
 *              port/), with the helper macros of the SDK that the app
 *              uses and the layer does not have yet.  (<objidl.h> first:
 *              the layer's commctrl.h needs LPSTREAM, which its
 *              <windows.h> gives only without WIN32_LEAN_AND_MEAN.)
 */
#ifndef DMODEM_WINPORT_COMMCTRL_H
#define DMODEM_WINPORT_COMMCTRL_H
#include <windows.h>
#include <objidl.h>
#include_next <commctrl.h>

#ifndef ListView_InsertColumn
#define ListView_InsertColumn(w, i, c) \
	(int)SendMessage((w), LVM_INSERTCOLUMN, (WPARAM)(int)(i), (LPARAM)(c))
#endif
#ifndef ListView_InsertItem
#define ListView_InsertItem(w, item) \
	(int)SendMessage((w), LVM_INSERTITEM, 0, (LPARAM)(item))
#endif
#ifndef ListView_GetItem
#define ListView_GetItem(w, item) \
	(BOOL)SendMessage((w), LVM_GETITEM, 0, (LPARAM)(item))
#endif
#ifndef ListView_DeleteAllItems
#define ListView_DeleteAllItems(w) \
	(BOOL)SendMessage((w), LVM_DELETEALLITEMS, 0, 0)
#endif
#ifndef ListView_GetNextItem
#define ListView_GetNextItem(w, i, flags) \
	(int)SendMessage((w), LVM_GETNEXTITEM, (WPARAM)(int)(i), \
			 MAKELPARAM((flags), 0))
#endif
#ifndef ListView_SetExtendedListViewStyle
#define ListView_SetExtendedListViewStyle(w, ex) \
	(DWORD)SendMessage((w), LVM_SETEXTENDEDLISTVIEWSTYLE, 0, (LPARAM)(ex))
#endif
#ifndef ListView_SetItemText
#define ListView_SetItemText(w, i, sub, text)                          \
	do {                                                           \
		LV_ITEM lvi_;                                          \
		lvi_.iSubItem = (sub);                                 \
		lvi_.pszText = (text);                                 \
		SendMessage((w), LVM_SETITEMTEXT, (WPARAM)(i),         \
			    (LPARAM)&lvi_);                            \
	} while (0)
#endif
#ifndef TabCtrl_InsertItem
#define TabCtrl_InsertItem(w, i, item) \
	(int)SendMessage((w), TCM_INSERTITEM, (WPARAM)(int)(i), (LPARAM)(item))
#endif
#ifndef TabCtrl_GetCurSel
#define TabCtrl_GetCurSel(w) (int)SendMessage((w), TCM_GETCURSEL, 0, 0)
#endif
#ifndef TabCtrl_AdjustRect
#define TabCtrl_AdjustRect(w, larger, rc) \
	(void)SendMessage((w), TCM_ADJUSTRECT, (WPARAM)(BOOL)(larger), (LPARAM)(rc))
#endif
#endif
