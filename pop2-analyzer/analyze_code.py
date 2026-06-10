#!/usr/bin/env python3
"""Analyze classic Mac 68k CODE resources: jump table, segment headers, A-line traps.

Usage:
    analyze_code.py <code_dir>          # dir with 0.bin, 1.bin / 1_Name.bin ...
    analyze_code.py <code_dir> --json out.json
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

# Toolbox/OS trap names (subset relevant to a mid-90s game; extend as needed).
# Source: Inside Macintosh trap tables (public Apple documentation).
TRAP_NAMES = {
    # --- OS traps (0xA0xx) ---
    0xA000: "_Open", 0xA001: "_Close", 0xA002: "_Read", 0xA003: "_Write",
    0xA004: "_Control", 0xA005: "_Status", 0xA008: "_Create", 0xA009: "_Delete",
    0xA00A: "_OpenRF", 0xA00B: "_Rename", 0xA00C: "_GetFileInfo", 0xA00D: "_SetFileInfo",
    0xA00F: "_MountVol", 0xA010: "_Allocate", 0xA011: "_GetEOF", 0xA012: "_SetEOF",
    0xA013: "_FlushVol", 0xA014: "_GetVol", 0xA015: "_SetVol", 0xA018: "_GetFPos",
    0xA019: "_InitZone", 0xA01B: "_SetZone", 0xA01C: "_FreeMem", 0xA01F: "_DisposePtr",
    0xA020: "_SetPtrSize", 0xA021: "_GetPtrSize", 0xA023: "_DisposeHandle",
    0xA024: "_SetHandleSize", 0xA025: "_GetHandleSize", 0xA029: "_HLock",
    0xA02A: "_HUnlock", 0xA02B: "_EmptyHandle", 0xA02C: "_InitApplZone",
    0xA02D: "_SetApplLimit", 0xA02E: "_BlockMove", 0xA031: "_GetOSEvent",
    0xA032: "_FlushEvents", 0xA036: "_MoreMasters", 0xA03B: "_Delay",
    0xA03C: "_CmpString", 0xA03F: "_InitUtil", 0xA040: "_ResrvMem",
    0xA044: "_SetFPos", 0xA047: "_SetTrapAddress", 0xA049: "_HPurge",
    0xA04A: "_HNoPurge", 0xA055: "_StripAddress", 0xA057: "_SetAppBase",
    0xA058: "_InsTime", 0xA059: "_RmvTime", 0xA05A: "_PrimeTime",
    0xA05D: "_SwapMMUMode",
    0xA05E: "_NMInstall", 0xA05F: "_NMRemove", 0xA060: "_FSDispatch",
    0xA061: "_MaxBlock", 0xA063: "_MaxApplZone", 0xA064: "_MoveHHi",
    0xA065: "_StackSpace", 0xA069: "_HGetState", 0xA06A: "_HSetState",
    0xA079: "_LockMemory", 0xA07A: "_UnlockMemory", 0xA07D: "_GetMemFragment",
    0xA08A: "_SlpQInstall",
    0xA11A: "_GetZone", 0xA11D: "_MaxMem", 0xA11E: "_NewPtr", 0xA122: "_NewHandle",
    0xA126: "_HandleZone", 0xA128: "_RecoverHandle", 0xA12F: "_PPostEvent",
    0xA146: "_GetTrapAddress", 0xA162: "_PurgeSpace",
    0xA1AD: "_Gestalt",
    0xA200: "_HOpen", 0xA207: "_HGetVInfo", 0xA208: "_HCreate", 0xA209: "_HDelete",
    0xA20A: "_HOpenRF", 0xA20B: "_HRename", 0xA20C: "_HGetFileInfo",
    0xA20D: "_HSetFileInfo", 0xA210: "_AllocContig", 0xA214: "_HGetVol",
    0xA215: "_HSetVol", 0xA241: "_HSetFLock",
    0xA346: "_GetOSTrapAddress", 0xA746: "_GetToolTrapAddress",
    0xA522: "_NewHandleClear", 0xA31E: "_NewPtrClear", 0xA51E: "_NewPtrSys",
    0xA440: "_ResrvMemSys", 0xA463: "_MaxApplZoneSys",
    # --- Event Manager / misc Toolbox ---
    0xA860: "_WaitNextEvent", 0xA970: "_GetNextEvent", 0xA971: "_EventAvail",
    0xA972: "_GetMouse", 0xA973: "_StillDown", 0xA974: "_Button",
    0xA975: "_TickCount", 0xA976: "_GetKeys", 0xA977: "_WaitMouseUp",
    0xA979: "_CouldDialog", 0xA97A: "_FreeDialog", 0xA97B: "_InitDialogs",
    0xA97C: "_GetNewDialog", 0xA97D: "_NewDialog", 0xA97E: "_SelIText",
    0xA97F: "_IsDialogEvent", 0xA980: "_DialogSelect", 0xA981: "_DrawDialog",
    0xA982: "_CloseDialog", 0xA983: "_DisposDialog", 0xA984: "_FindDItem",
    0xA985: "_Alert",
    0xA986: "_StopAlert", 0xA987: "_NoteAlert", 0xA988: "_CautionAlert",
    0xA989: "_CouldAlert", 0xA98A: "_FreeAlert", 0xA98B: "_ParamText",
    0xA98C: "_ErrorSound",
    0xA98D: "_GetDItem", 0xA98E: "_SetDItem", 0xA98F: "_SetIText",
    0xA990: "_GetIText",
    0xA991: "_ModalDialog", 0xA992: "_DetachResource",
    0xA993: "_SetResPurge", 0xA994: "_CurResFile", 0xA995: "_InitResources",
    0xA996: "_RsrcZoneInit", 0xA997: "_OpenResFile", 0xA998: "_UseResFile",
    0xA999: "_UpdateResFile", 0xA99A: "_CloseResFile", 0xA99B: "_SetResLoad",
    0xA99C: "_CountResources", 0xA99D: "_GetIndResource", 0xA99E: "_CountTypes",
    0xA99F: "_GetIndType",
    0xA9A0: "_GetResource", 0xA9A1: "_GetNamedResource", 0xA9A2: "_LoadResource",
    0xA9A3: "_ReleaseResource", 0xA9A4: "_HomeResFile", 0xA9A5: "_SizeRsrc",
    0xA9A6: "_GetResAttrs", 0xA9A7: "_SetResAttrs", 0xA9A8: "_GetResInfo",
    0xA9A9: "_SetResInfo", 0xA9AA: "_ChangedResource", 0xA9AB: "_AddResource",
    0xA9AD: "_RemoveResource", 0xA9AF: "_ResError", 0xA9B0: "_WriteResource",
    0xA9B1: "_CreateResFile", 0xA9B2: "_SystemEvent", 0xA9B3: "_SystemClick",
    0xA9B4: "_SystemTask", 0xA9B5: "_SystemMenu", 0xA9B6: "_OpenDeskAcc",
    0xA9B7: "_CloseDeskAcc", 0xA9B8: "_GetPattern", 0xA9B9: "_GetCursor",
    0xA9BA: "_GetString", 0xA9BB: "_GetIcon", 0xA9BC: "_GetPicture",
    0xA9BD: "_GetNewWindow", 0xA9BE: "_GetNewControl", 0xA9BF: "_GetRMenu",
    0xA9C0: "_GetNewMBar", 0xA9C1: "_UniqueID", 0xA9C2: "_SysEdit",
    0xA9C6: "_Secs2Date", 0xA9C7: "_Date2Secs", 0xA9C8: "_SysBeep",
    0xA9C9: "_SysError", 0xA9CB: "_TEGetText", 0xA9CC: "_TEInit",
    0xA9CD: "_TEDispose", 0xA9D2: "_TESetText", 0xA9D3: "_TECalText",
    0xA9D6: "_TEUpdate", 0xA9DA: "_TEScroll", 0xA9DF: "_TEActivate",
    0xA9E0: "_TEDeactivate", 0xA9E1: "_TEKey", 0xA9E2: "_TECut",
    0xA9E3: "_TECopy", 0xA9E4: "_TEPaste",
    0xA9EB: "_FP68K", 0xA9EC: "_Elems68K",
    0xA9EE: "_DECSTR68K", 0xA9F0: "_LoadSeg", 0xA9F1: "_UnloadSeg",
    0xA9F2: "_Launch", 0xA9F3: "_Chain", 0xA9F4: "_ExitToShell",
    0xA9F5: "_GetAppParms", 0xA9F6: "_GetResFileAttrs", 0xA9F7: "_SetResFileAttrs",
    0xA9F9: "_InfoScrap", 0xA9FA: "_UnlodeScrap", 0xA9FB: "_LodeScrap",
    0xA9FC: "_ZeroScrap", 0xA9FD: "_GetScrap", 0xA9FE: "_PutScrap",
    # --- QuickDraw ---
    0xA850: "_InitCursor", 0xA851: "_SetCursor", 0xA852: "_HideCursor",
    0xA853: "_ShowCursor", 0xA855: "_ShieldCursor", 0xA856: "_ObscureCursor",
    0xA858: "_BitAnd", 0xA859: "_BitXor", 0xA85A: "_BitNot", 0xA85B: "_BitOr",
    0xA85C: "_BitShift", 0xA85D: "_BitTst", 0xA85E: "_BitSet", 0xA85F: "_BitClr",
    0xA861: "_Random", 0xA862: "_ForeColor", 0xA863: "_BackColor",
    0xA864: "_ColorBit", 0xA865: "_GetPixel", 0xA866: "_StuffHex",
    0xA867: "_LongMul", 0xA868: "_FixMul", 0xA869: "_FixRatio",
    0xA86A: "_HiWord", 0xA86B: "_LoWord", 0xA86C: "_FixRound",
    0xA86D: "_InitPort", 0xA86E: "_InitGraf", 0xA86F: "_OpenPort",
    0xA870: "_LocalToGlobal", 0xA871: "_GlobalToLocal", 0xA872: "_GrafDevice",
    0xA873: "_SetPort", 0xA874: "_GetPort", 0xA875: "_SetPBits",
    0xA876: "_PortSize", 0xA877: "_MovePortTo", 0xA878: "_SetOrigin",
    0xA879: "_SetClip", 0xA87A: "_GetClip", 0xA87B: "_ClipRect",
    0xA87C: "_BackPat", 0xA87D: "_ClosePort", 0xA87E: "_AddPt",
    0xA87F: "_SubPt", 0xA880: "_SetPt", 0xA881: "_EqualPt",
    0xA882: "_StdText", 0xA883: "_DrawChar", 0xA884: "_DrawString",
    0xA885: "_DrawText", 0xA886: "_TextWidth", 0xA887: "_TextFont",
    0xA888: "_TextFace", 0xA889: "_TextMode", 0xA88A: "_TextSize",
    0xA88B: "_GetFontInfo", 0xA88C: "_StringWidth", 0xA88D: "_CharWidth",
    0xA88E: "_SpaceExtra", 0xA890: "_StdLine", 0xA891: "_LineTo",
    0xA892: "_Line", 0xA893: "_MoveTo", 0xA894: "_Move",
    0xA896: "_HidePen", 0xA897: "_ShowPen", 0xA898: "_GetPenState",
    0xA899: "_SetPenState", 0xA89A: "_GetPen", 0xA89B: "_PenSize",
    0xA89C: "_PenMode", 0xA89D: "_PenPat", 0xA89E: "_PenNormal",
    0xA8A0: "_StdRect", 0xA8A1: "_FrameRect", 0xA8A2: "_PaintRect",
    0xA8A3: "_EraseRect", 0xA8A4: "_InverRect", 0xA8A5: "_FillRect",
    0xA8A6: "_EqualRect", 0xA8A7: "_SetRect", 0xA8A8: "_OffsetRect",
    0xA8A9: "_InsetRect", 0xA8AA: "_SectRect", 0xA8AB: "_UnionRect",
    0xA8AC: "_Pt2Rect", 0xA8AD: "_PtInRect", 0xA8AE: "_EmptyRect",
    0xA8AF: "_StdRRect", 0xA8B0: "_FrameRoundRect", 0xA8B1: "_PaintRoundRect",
    0xA8B2: "_EraseRoundRect", 0xA8B3: "_InverRoundRect", 0xA8B4: "_FillRoundRect",
    0xA8B6: "_StdOval", 0xA8B7: "_FrameOval", 0xA8B8: "_PaintOval",
    0xA8B9: "_EraseOval", 0xA8BA: "_InvertOval", 0xA8BB: "_FillOval",
    0xA8BD: "_StdArc", 0xA8BE: "_FrameArc", 0xA8BF: "_PaintArc",
    0xA8C0: "_EraseArc", 0xA8C1: "_InvertArc", 0xA8C2: "_FillArc",
    0xA8C3: "_PtToAngle", 0xA8C5: "_StdPoly", 0xA8C6: "_FramePoly",
    0xA8C7: "_PaintPoly", 0xA8C8: "_ErasePoly", 0xA8C9: "_InvertPoly",
    0xA8CA: "_FillPoly", 0xA8CB: "_OpenPoly", 0xA8CC: "_ClosePgon",
    0xA8CD: "_KillPoly", 0xA8CE: "_OffsetPoly", 0xA8CF: "_PackBits",
    0xA8D0: "_UnpackBits", 0xA8D1: "_StdRgn", 0xA8D2: "_FrameRgn",
    0xA8D3: "_PaintRgn", 0xA8D4: "_EraseRgn", 0xA8D5: "_InverRgn",
    0xA8D6: "_FillRgn", 0xA8D8: "_NewRgn", 0xA8D9: "_DisposRgn",
    0xA8DA: "_OpenRgn", 0xA8DB: "_CloseRgn", 0xA8DC: "_CopyRgn",
    0xA8DD: "_SetEmptyRgn", 0xA8DE: "_SetRecRgn", 0xA8DF: "_RectRgn",
    0xA8E0: "_OfsetRgn", 0xA8E1: "_InsetRgn", 0xA8E2: "_EmptyRgn",
    0xA8E3: "_EqualRgn", 0xA8E4: "_SectRgn", 0xA8E5: "_UnionRgn",
    0xA8E6: "_DiffRgn", 0xA8E7: "_XorRgn", 0xA8E8: "_PtInRgn",
    0xA8E9: "_RectInRgn", 0xA8EA: "_SetStdProcs", 0xA8EB: "_StdBits",
    0xA8EC: "_CopyBits", 0xA8ED: "_StdTxMeas", 0xA8EE: "_StdGetPic",
    0xA8EF: "_ScrollRect", 0xA8F0: "_StdPutPic", 0xA8F1: "_StdComment",
    0xA8F2: "_PicComment", 0xA8F3: "_OpenPicture", 0xA8F4: "_ClosePicture",
    0xA8F5: "_KillPicture", 0xA8F6: "_DrawPicture", 0xA8F8: "_ScalePt",
    0xA8F9: "_MapPt", 0xA8FA: "_MapRect", 0xA8FB: "_MapRgn",
    0xA8FC: "_MapPoly", 0xA8FE: "_InitFonts", 0xA8FF: "_GetFName",
    0xA900: "_GetFNum", 0xA901: "_FMSwapFont", 0xA902: "_RealFont",
    0xA903: "_SetFontLock", 0xA904: "_DrawGrowIcon", 0xA905: "_DragGrayRgn",
    0xA906: "_NewString", 0xA907: "_SetString", 0xA908: "_ShowHide",
    0xA909: "_CalcVis", 0xA90A: "_CalcVBehind", 0xA90B: "_ClipAbove",
    0xA90C: "_PaintOne", 0xA90D: "_PaintBehind", 0xA90E: "_SaveOld",
    0xA90F: "_DrawNew", 0xA910: "_GetWMgrPort", 0xA911: "_CheckUpdate",
    0xA912: "_InitWindows", 0xA913: "_NewWindow", 0xA914: "_DisposWindow",
    0xA915: "_ShowWindow", 0xA916: "_HideWindow", 0xA917: "_GetWRefCon",
    0xA918: "_SetWRefCon", 0xA919: "_GetWTitle", 0xA91A: "_SetWTitle",
    0xA91B: "_MoveWindow", 0xA91C: "_HiliteWindow", 0xA91D: "_SizeWindow",
    0xA91E: "_TrackGoAway", 0xA91F: "_SelectWindow", 0xA920: "_BringToFront",
    0xA921: "_SendBehind", 0xA922: "_BeginUpdate", 0xA923: "_EndUpdate",
    0xA924: "_FrontWindow", 0xA925: "_DragWindow", 0xA926: "_DragTheRgn",
    0xA927: "_InvalRgn", 0xA928: "_InvalRect", 0xA929: "_ValidRgn",
    0xA92A: "_ValidRect", 0xA92B: "_GrowWindow", 0xA92C: "_FindWindow",
    0xA92D: "_CloseWindow", 0xA92E: "_SetWindowPic", 0xA92F: "_GetWindowPic",
    0xA930: "_InitMenus", 0xA931: "_NewMenu", 0xA932: "_DisposMenu",
    0xA933: "_AppendMenu", 0xA934: "_ClearMenuBar", 0xA935: "_InsertMenu",
    0xA936: "_DeleteMenu", 0xA937: "_DrawMenuBar", 0xA938: "_HiliteMenu",
    0xA939: "_EnableItem", 0xA93A: "_DisableItem", 0xA93B: "_GetMenuBar",
    0xA93C: "_SetMenuBar", 0xA93D: "_MenuSelect", 0xA93E: "_MenuKey",
    0xA93F: "_GetItmIcon", 0xA940: "_SetItmIcon", 0xA941: "_GetItmStyle",
    0xA942: "_SetItmStyle", 0xA943: "_GetItmMark", 0xA944: "_SetItmMark",
    0xA945: "_CheckItem", 0xA946: "_GetItem", 0xA947: "_SetItem",
    0xA948: "_CalcMenuSize", 0xA949: "_GetMHandle", 0xA94A: "_SetMFlash",
    0xA94B: "_PlotIcon", 0xA94C: "_FlashMenuBar", 0xA94D: "_AddResMenu",
    0xA94E: "_PinRect", 0xA94F: "_DeltaPoint", 0xA950: "_CountMItems",
    0xA951: "_InsertResMenu", 0xA954: "_NewControl", 0xA955: "_DisposControl",
    0xA956: "_KillControls", 0xA957: "_ShowControl", 0xA958: "_HideControl",
    0xA959: "_MoveControl", 0xA95A: "_GetCRefCon", 0xA95B: "_SetCRefCon",
    0xA95C: "_SizeControl", 0xA95D: "_HiliteControl", 0xA95E: "_GetCTitle",
    0xA95F: "_SetCTitle", 0xA960: "_GetCtlValue", 0xA961: "_GetMinCtl",
    0xA962: "_GetMaxCtl", 0xA963: "_SetCtlValue", 0xA964: "_SetMinCtl",
    0xA965: "_SetMaxCtl", 0xA966: "_TestControl", 0xA967: "_DragControl",
    0xA968: "_TrackControl", 0xA969: "_DrawControls", 0xA96A: "_GetCtlAction",
    0xA96B: "_SetCtlAction", 0xA96C: "_FindControl", 0xA96E: "_Dequeue",
    0xA96F: "_Enqueue",
    # --- Color QuickDraw / Palette ---
    0xAA00: "_OpenCPort", 0xAA01: "_InitCPort", 0xAA03: "_NewPixMap",
    0xAA04: "_DisposPixMap", 0xAA05: "_CopyPixMap", 0xAA06: "_SetPortPix",
    0xAA07: "_NewPixPat", 0xAA08: "_DisposPixPat", 0xAA09: "_CopyPixPat",
    0xAA0A: "_PenPixPat", 0xAA0B: "_BackPixPat", 0xAA0C: "_GetPixPat",
    0xAA0D: "_MakeRGBPat", 0xAA0E: "_FillCRect", 0xAA0F: "_FillCOval",
    0xAA14: "_RGBForeColor", 0xAA15: "_RGBBackColor", 0xAA16: "_SetCPixel",
    0xAA17: "_GetCPixel", 0xAA18: "_GetCTable", 0xAA19: "_GetForeColor",
    0xAA1A: "_GetBackColor", 0xAA1B: "_GetCCursor", 0xAA1C: "_SetCCursor",
    0xAA1D: "_AllocCursor", 0xAA1E: "_GetCIcon", 0xAA1F: "_PlotCIcon",
    0xAA20: "_OpenCPicture", 0xAA21: "_OpColor", 0xAA22: "_HiliteColor",
    0xAA23: "_CharExtra", 0xAA24: "_DisposCTable", 0xAA25: "_DisposCIcon",
    0xAA26: "_DisposCCursor",
    0xAA27: "_GetMaxDevice", 0xAA28: "_GetCTSeed", 0xAA29: "_GetDeviceList",
    0xAA2A: "_GetMainDevice", 0xAA2B: "_GetNextDevice",
    0xAA2C: "_TestDeviceAttribute", 0xAA2D: "_SetDeviceAttribute",
    0xAA2E: "_InitGDevice", 0xAA2F: "_NewGDevice",
    0xAA30: "_DisposGDevice", 0xAA31: "_SetGDevice", 0xAA32: "_GetGDevice",
    0xAA35: "_CalcCMask",
    0xAA36: "_SeedCFill", 0xAA39: "_MakeITable", 0xAA3A: "_AddSearch",
    0xAA3B: "_AddComp", 0xAA3C: "_SetClientID", 0xAA3D: "_ProtectEntry",
    0xAA3E: "_ReserveEntry", 0xAA3F: "_SetEntries", 0xAA40: "_QDError",
    0xAA41: "_SetWinColor", 0xAA42: "_GetAuxWin", 0xAA43: "_SetCtlColor",
    0xAA44: "_GetAuxiliaryCtlRec", 0xAA45: "_NewCMenu", 0xAA46: "_DispMCInfo",
    0xAA47: "_SetMCInfo", 0xAA48: "_GetMCInfo", 0xAA49: "_SetMCEntries",
    0xAA4A: "_GetMCEntry", 0xAA4B: "_SetFractEnable", 0xAA4C: "_SetFamilyID",
    0xAA4D: "_GetItemCmd", 0xAA4E: "_SetItemCmd",
    # 0xAA4F-0xAA68: unverified — names removed after shift bugs; resolve on use
    0xAA60: "_DelMCEntries", 0xAA61: "_GetCTSeed",
    # Palette Manager (canonical range)
    0xAA90: "_InitPalettes", 0xAA91: "_NewPalette", 0xAA92: "_GetNewPalette",
    0xAA93: "_DisposePalette", 0xAA94: "_ActivatePalette", 0xAA95: "_SetPalette",
    0xAA96: "_GetPalette", 0xAA97: "_PmForeColor", 0xAA98: "_PmBackColor",
    0xAA99: "_AnimateEntry", 0xAA9A: "_AnimatePalette", 0xAA9B: "_GetEntryColor",
    0xAA9C: "_SetEntryColor", 0xAA9D: "_GetEntryUsage", 0xAA9E: "_SetEntryUsage",
    0xAA9F: "_CTab2Palette", 0xAAA0: "_Palette2CTab", 0xAAA1: "_CopyPalette",
    0xAAA2: "_PaletteDispatch",
    # --- Sound Manager ---
    0xA800: "_SoundDispatch", 0xA801: "_SndDisposeChannel", 0xA802: "_SndAddModifier",
    0xA803: "_SndDoCommand", 0xA804: "_SndDoImmediate", 0xA805: "_SndPlay",
    0xA806: "_SndControl", 0xA807: "_SndNewChannel", 0xA80E: "_SndStopFilePlay",
    0xA80F: "_MACEVersion", 0xA810: "_SPBVersion", 0xA811: "_SndSoundManagerVersion",
    # --- Misc / managers ---
    0xA808: "_InitProcMenu", 0xA809: "_GetCVariant", 0xA80A: "_GetWVariant",
    0xA80B: "_PopUpMenuSelect", 0xA80C: "_RGetResource",
    0xA815: "_SCSIDispatch", 0xA817: "_CopyMask", 0xA819: "_XMunger",
    0xA81A: "_HOpenResFile", 0xA81B: "_HCreateResFile",
    0xA81D: "_InvalMenuBar", 0xA81F: "_GetCompressedPixMapInfo",
    0xA820: "_FindSpecialFolder", 0xA821: "_MaxSizeRsrc",
    0xA822: "_ResourceDispatch", 0xA823: "_AliasDispatch",
    0xA826: "_InsertMenuItem", 0xA827: "_HideDItem", 0xA828: "_ShowDItem",
    0xA82A: "_ComponentDispatch",
    0xA833: "_ScrnBitMap", 0xA834: "_SetFScaleDisable", 0xA835: "_FontMetrics",
    0xA836: "_GetMaskTable", 0xA837: "_MeasureText", 0xA838: "_CalcMask",
    0xA839: "_SeedFill", 0xA83A: "_ZoomWindow", 0xA83B: "_TrackBox",
    0xA83C: "_TEGetOffset", 0xA83D: "_TEDispatch", 0xA83E: "_TEStyleNew",
    0xA847: "_FracCos", 0xA848: "_FracSin", 0xA849: "_FracSqrt",
    0xA84A: "_FracMul", 0xA84B: "_FracDiv", 0xA84D: "_FixDiv",
    0xA84E: "_GetItemStyle", 0xA84F: "_SetItemStyle",
    0xA854: "_UprString", 0xA8B5: "_ScriptUtil",
    0xA88F: "_OSDispatch", 0xA895: "_ShutDown",
    0xAB1D: "_QDExtensions",
    0xA090: "_SysEnvirons",
    0xABC9: "_IconDispatch", 0xABCA: "_DeviceLoop",
    0xABEA: "_DictionaryDispatch",
    0xABF2: "_ThreadDispatch", 0xABF6: "_DebugStr",
    0xA9FF: "_Debugger",
    0xA8FD: "_PrGlue", 0xA9CE: "_TEIdle", 0xA9CF: "_TEClick",
    0xA9D0: "_TESetSelect", 0xA9D1: "_TENew", 0xA9D4: "_TESetJust",
    0xA9D5: "_TEPinScroll", 0xA9D7: "_TEDelete", 0xA9D8: "_TEInsert",
    0xA9D9: "_TEStylePaste",
}
# Remove accidental bogus keys (defensive: ints only)
TRAP_NAMES = {k: v for k, v in TRAP_NAMES.items() if isinstance(k, int)}


def trap_name(op: int) -> str:
    if op in TRAP_NAMES:
        return TRAP_NAMES[op]
    # Toolbox traps: bit 10 set => 0xA800-0xABFF; auto-pop bit 0x400
    if 0xAC00 <= op <= 0xAFFF and (op & ~0x0400) in TRAP_NAMES:
        return TRAP_NAMES[op & ~0x0400] + ",autopop"
    # OS traps: bits 8-10 are flags
    if op < 0xA800 and (op & 0xA0FF) in TRAP_NAMES:
        flags = (op >> 8) & 7
        return TRAP_NAMES[op & 0xA0FF] + f",f{flags}"
    return f"unknown_{op:04X}"


def load_segments(code_dir: Path):
    segs = {}
    for f in code_dir.glob("*.bin"):
        m = re.match(r"^(-?\d+)(?:_(.*))?\.bin$", f.name)
        if not m:
            continue
        segs[int(m.group(1))] = (m.group(2) or "", f.read_bytes())
    return dict(sorted(segs.items()))


def parse_code0(data: bytes):
    above, below, jt_size, jt_off = struct.unpack_from(">IIII", data, 0)
    entries = []
    for i in range(jt_size // 8):
        off = 16 + i * 8
        roff, push, seg, trap = struct.unpack_from(">HHHH", data, off)
        if push == 0x3F3C and trap == 0xA9F0:
            # callers target the executable part of the entry: base + 2
            entries.append({"jt": i, "a5off": 32 + i * 8, "a5call": 32 + i * 8 + 2,
                            "seg": seg, "off": roff})
        else:
            entries.append({"jt": i, "a5off": 32 + i * 8, "raw": data[off:off+8].hex()})
    return {"aboveA5": above, "belowA5": below, "jtSize": jt_size,
            "jtOffset": jt_off, "entries": entries}


def scan_traps(code: bytes, start: int):
    """Linear sweep for A-line opcodes. Approximate (data-in-code may add noise)."""
    found = {}
    for off in range(start, len(code) - 1, 2):
        w = (code[off] << 8) | code[off + 1]
        if 0xA000 <= w <= 0xAFFF:
            found.setdefault(w, []).append(off)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    segs = load_segments(args.code_dir)
    if 0 not in segs:
        sys.exit("no CODE 0 found")

    c0 = parse_code0(segs[0][1])
    print(f"CODE 0: aboveA5={c0['aboveA5']} belowA5={c0['belowA5']} "
          f"jtEntries={c0['jtSize']//8}")

    per_seg_jt = {}
    for e in c0["entries"]:
        if "seg" in e:
            per_seg_jt.setdefault(e["seg"], []).append(e)

    all_traps = {}
    seg_report = []
    for sid, (name, data) in segs.items():
        if sid == 0:
            continue
        first_jt, n_jt = struct.unpack_from(">HH", data, 0)
        far = first_jt == 0xFFFF
        hdr = {"seg": sid, "name": name, "size": len(data),
               "far_model": far, "jt_first": first_jt, "jt_count": n_jt,
               "jt_in_code0": len(per_seg_jt.get(sid, []))}
        traps = scan_traps(data, 4)
        hdr["trap_count"] = sum(len(v) for v in traps.values())
        hdr["trap_unique"] = len(traps)
        seg_report.append(hdr)
        for op, offs in traps.items():
            all_traps.setdefault(op, []).extend((sid, o) for o in offs)

    print(f"\n{'seg':>3} {'name':<16} {'size':>6} {'jt#':>4} "
          f"{'jtC0':>4} {'traps':>5} {'uniq':>4} model")
    for h in seg_report:
        print(f"{h['seg']:>3} {h['name']:<16.16} {h['size']:>6} "
              f"{h['jt_count']:>4} {h['jt_in_code0']:>4} "
              f"{h['trap_count']:>5} {h['trap_unique']:>4} "
              f"{'far' if h['far_model'] else 'near'}")

    print(f"\nTotal unique A-line opcodes: {len(all_traps)}")
    print(f"{'opcode':<8} {'count':>5}  name")
    for op in sorted(all_traps, key=lambda o: -len(all_traps[o])):
        print(f"0x{op:04X}  {len(all_traps[op]):>5}  {trap_name(op)}")

    if args.json:
        out = {
            "code0": c0,
            "segments": seg_report,
            "traps": {
                f"0x{op:04X}": {
                    "name": trap_name(op),
                    "count": len(sites),
                    "sites": [{"seg": s, "off": o} for s, o in sites],
                }
                for op, sites in sorted(all_traps.items())
            },
        }
        args.json.write_text(json.dumps(out, indent=1))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
