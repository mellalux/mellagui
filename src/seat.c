/*
 * wl_seat (pointer, keyboard, touch), wl_data_device_manager (text clipboard,
 * drag-and-drop is accepted but cancelled), cursor handling and pointer grabs.
 *
 * Everything runs on the single event-loop thread. Surfaces that hold focus
 * are watched through their destroy_signal; wl_pointer/wl_keyboard/wl_touch/
 * wl_data_* resources are kept in per-seat lists so events can be fanned out
 * to every object a client created.
 */


#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "keymap.h"
#include "log.h"
#include "output.h"
#include "protocol.h"
#include "render.h"
#include "seat.h"
#include "server.h"
#include "surface.h"
#include "wm.h"

#define MAX_KEYS        64
#define MAX_BUTTONS     16
#define MAX_SERIALS     32
#define MAX_TOUCH       10
#define MAX_MIMES       64
#define MAX_MIME_LEN    255
#define SEAT_NAME       "seat0"

/* ---- embedded XKB keymap (US layout) ------------------------------------------------ */

/*
 * A complete xkb_keymap in text form: evdev keycodes (xkb code = evdev + 8),
 * the types, compat rules and symbols of the "pc105 + us" layout, with the
 * usual real-modifier mapping (Shift=0 Lock=1 Control=2 Mod1/Alt=3
 * Mod2/NumLock=4 Mod4/Super=6). Self-contained: no `include` statements.
 * Verified to compile with libxkbcommon and to produce the same keysyms as
 * the stock evdev/us keymap for all keys at Shift, Caps, NumLock, Ctrl, Alt
 * and Super states.
 */
static const char lumen_keymap_text[] =
    "xkb_keymap {\n"
    "xkb_keycodes \"lumen\" {\n"
    "  minimum = 8;\n"
    "  maximum = 255;\n"
    "  <ESC> = 9;\n"
    "  <AE01> = 10;\n"
    "  <AE02> = 11;\n"
    "  <AE03> = 12;\n"
    "  <AE04> = 13;\n"
    "  <AE05> = 14;\n"
    "  <AE06> = 15;\n"
    "  <AE07> = 16;\n"
    "  <AE08> = 17;\n"
    "  <AE09> = 18;\n"
    "  <AE10> = 19;\n"
    "  <AE11> = 20;\n"
    "  <AE12> = 21;\n"
    "  <BKSP> = 22;\n"
    "  <TAB> = 23;\n"
    "  <AD01> = 24;\n"
    "  <AD02> = 25;\n"
    "  <AD03> = 26;\n"
    "  <AD04> = 27;\n"
    "  <AD05> = 28;\n"
    "  <AD06> = 29;\n"
    "  <AD07> = 30;\n"
    "  <AD08> = 31;\n"
    "  <AD09> = 32;\n"
    "  <AD10> = 33;\n"
    "  <AD11> = 34;\n"
    "  <AD12> = 35;\n"
    "  <RTRN> = 36;\n"
    "  <LCTL> = 37;\n"
    "  <AC01> = 38;\n"
    "  <AC02> = 39;\n"
    "  <AC03> = 40;\n"
    "  <AC04> = 41;\n"
    "  <AC05> = 42;\n"
    "  <AC06> = 43;\n"
    "  <AC07> = 44;\n"
    "  <AC08> = 45;\n"
    "  <AC09> = 46;\n"
    "  <AC10> = 47;\n"
    "  <AC11> = 48;\n"
    "  <TLDE> = 49;\n"
    "  <LFSH> = 50;\n"
    "  <BKSL> = 51;\n"
    "  <AB01> = 52;\n"
    "  <AB02> = 53;\n"
    "  <AB03> = 54;\n"
    "  <AB04> = 55;\n"
    "  <AB05> = 56;\n"
    "  <AB06> = 57;\n"
    "  <AB07> = 58;\n"
    "  <AB08> = 59;\n"
    "  <AB09> = 60;\n"
    "  <AB10> = 61;\n"
    "  <RTSH> = 62;\n"
    "  <KPMU> = 63;\n"
    "  <LALT> = 64;\n"
    "  <SPCE> = 65;\n"
    "  <CAPS> = 66;\n"
    "  <FK01> = 67;\n"
    "  <FK02> = 68;\n"
    "  <FK03> = 69;\n"
    "  <FK04> = 70;\n"
    "  <FK05> = 71;\n"
    "  <FK06> = 72;\n"
    "  <FK07> = 73;\n"
    "  <FK08> = 74;\n"
    "  <FK09> = 75;\n"
    "  <FK10> = 76;\n"
    "  <NMLK> = 77;\n"
    "  <SCLK> = 78;\n"
    "  <KP7> = 79;\n"
    "  <KP8> = 80;\n"
    "  <KP9> = 81;\n"
    "  <KPSU> = 82;\n"
    "  <KP4> = 83;\n"
    "  <KP5> = 84;\n"
    "  <KP6> = 85;\n"
    "  <KPAD> = 86;\n"
    "  <KP1> = 87;\n"
    "  <KP2> = 88;\n"
    "  <KP3> = 89;\n"
    "  <KP0> = 90;\n"
    "  <KPDL> = 91;\n"
    "  <LVL3> = 92;\n"
    "  <LSGT> = 94;\n"
    "  <FK11> = 95;\n"
    "  <FK12> = 96;\n"
    "  <AB11> = 97;\n"
    "  <KATA> = 98;\n"
    "  <HIRA> = 99;\n"
    "  <HENK> = 100;\n"
    "  <HKTG> = 101;\n"
    "  <MUHE> = 102;\n"
    "  <JPCM> = 103;\n"
    "  <KPEN> = 104;\n"
    "  <RCTL> = 105;\n"
    "  <KPDV> = 106;\n"
    "  <PRSC> = 107;\n"
    "  <RALT> = 108;\n"
    "  <LNFD> = 109;\n"
    "  <HOME> = 110;\n"
    "  <UP> = 111;\n"
    "  <PGUP> = 112;\n"
    "  <LEFT> = 113;\n"
    "  <RGHT> = 114;\n"
    "  <END> = 115;\n"
    "  <DOWN> = 116;\n"
    "  <PGDN> = 117;\n"
    "  <INS> = 118;\n"
    "  <DELE> = 119;\n"
    "  <I120> = 120;\n"
    "  <MUTE> = 121;\n"
    "  <VOL-> = 122;\n"
    "  <VOL+> = 123;\n"
    "  <POWR> = 124;\n"
    "  <KPEQ> = 125;\n"
    "  <I126> = 126;\n"
    "  <PAUS> = 127;\n"
    "  <I128> = 128;\n"
    "  <I129> = 129;\n"
    "  <HNGL> = 130;\n"
    "  <HJCV> = 131;\n"
    "  <AE13> = 132;\n"
    "  <LWIN> = 133;\n"
    "  <RWIN> = 134;\n"
    "  <COMP> = 135;\n"
    "  <STOP> = 136;\n"
    "  <AGAI> = 137;\n"
    "  <PROP> = 138;\n"
    "  <UNDO> = 139;\n"
    "  <FRNT> = 140;\n"
    "  <COPY> = 141;\n"
    "  <OPEN> = 142;\n"
    "  <PAST> = 143;\n"
    "  <FIND> = 144;\n"
    "  <CUT> = 145;\n"
    "  <HELP> = 146;\n"
    "  <I147> = 147;\n"
    "  <I148> = 148;\n"
    "  <I149> = 149;\n"
    "  <I150> = 150;\n"
    "  <I151> = 151;\n"
    "  <I152> = 152;\n"
    "  <I153> = 153;\n"
    "  <I154> = 154;\n"
    "  <I155> = 155;\n"
    "  <I156> = 156;\n"
    "  <I157> = 157;\n"
    "  <I158> = 158;\n"
    "  <I159> = 159;\n"
    "  <I160> = 160;\n"
    "  <I161> = 161;\n"
    "  <I162> = 162;\n"
    "  <I163> = 163;\n"
    "  <I164> = 164;\n"
    "  <I165> = 165;\n"
    "  <I166> = 166;\n"
    "  <I167> = 167;\n"
    "  <I168> = 168;\n"
    "  <I169> = 169;\n"
    "  <I170> = 170;\n"
    "  <I171> = 171;\n"
    "  <I172> = 172;\n"
    "  <I173> = 173;\n"
    "  <I174> = 174;\n"
    "  <I175> = 175;\n"
    "  <I176> = 176;\n"
    "  <I177> = 177;\n"
    "  <I178> = 178;\n"
    "  <I179> = 179;\n"
    "  <I180> = 180;\n"
    "  <I181> = 181;\n"
    "  <I182> = 182;\n"
    "  <I183> = 183;\n"
    "  <I184> = 184;\n"
    "  <I185> = 185;\n"
    "  <I186> = 186;\n"
    "  <I187> = 187;\n"
    "  <I188> = 188;\n"
    "  <I189> = 189;\n"
    "  <I190> = 190;\n"
    "  <FK13> = 191;\n"
    "  <FK14> = 192;\n"
    "  <FK15> = 193;\n"
    "  <FK16> = 194;\n"
    "  <FK17> = 195;\n"
    "  <FK18> = 196;\n"
    "  <FK19> = 197;\n"
    "  <FK20> = 198;\n"
    "  <FK21> = 199;\n"
    "  <FK22> = 200;\n"
    "  <FK23> = 201;\n"
    "  <FK24> = 202;\n"
    "  <LVL5> = 203;\n"
    "  <ALT> = 204;\n"
    "  <META> = 205;\n"
    "  <SUPR> = 206;\n"
    "  <HYPR> = 207;\n"
    "  <I208> = 208;\n"
    "  <I209> = 209;\n"
    "  <I210> = 210;\n"
    "  <I211> = 211;\n"
    "  <I212> = 212;\n"
    "  <I213> = 213;\n"
    "  <I214> = 214;\n"
    "  <I215> = 215;\n"
    "  <I216> = 216;\n"
    "  <I217> = 217;\n"
    "  <I218> = 218;\n"
    "  <I219> = 219;\n"
    "  <I220> = 220;\n"
    "  <I221> = 221;\n"
    "  <I222> = 222;\n"
    "  <I223> = 223;\n"
    "  <I224> = 224;\n"
    "  <I225> = 225;\n"
    "  <I226> = 226;\n"
    "  <I227> = 227;\n"
    "  <I228> = 228;\n"
    "  <I229> = 229;\n"
    "  <I230> = 230;\n"
    "  <I231> = 231;\n"
    "  <I232> = 232;\n"
    "  <I233> = 233;\n"
    "  <I234> = 234;\n"
    "  <I235> = 235;\n"
    "  <I236> = 236;\n"
    "  <I237> = 237;\n"
    "  <I238> = 238;\n"
    "  <I239> = 239;\n"
    "  <I240> = 240;\n"
    "  <I241> = 241;\n"
    "  <I242> = 242;\n"
    "  <I243> = 243;\n"
    "  <I244> = 244;\n"
    "  <I245> = 245;\n"
    "  <I246> = 246;\n"
    "  <I247> = 247;\n"
    "  <I248> = 248;\n"
    "  <I249> = 249;\n"
    "  <I250> = 250;\n"
    "  <I251> = 251;\n"
    "  <I252> = 252;\n"
    "  <I253> = 253;\n"
    "  <I254> = 254;\n"
    "  <I255> = 255;\n"
    "  indicator 1 = \"Caps Lock\";\n"
    "  indicator 2 = \"Num Lock\";\n"
    "  indicator 3 = \"Scroll Lock\";\n"
    "  alias <AC12> = <BKSL>;\n"
    "  alias <ALGR> = <RALT>;\n"
    "  alias <MENU> = <COMP>;\n"
    "  alias <HZTG> = <TLDE>;\n"
    "  alias <LMTA> = <LWIN>;\n"
    "  alias <RMTA> = <RWIN>;\n"
    "  alias <OUTP> = <I235>;\n"
    "  alias <KITG> = <I236>;\n"
    "  alias <KIDN> = <I237>;\n"
    "  alias <KIUP> = <I238>;\n"
    "  alias <I121> = <MUTE>;\n"
    "  alias <I122> = <VOL->;\n"
    "  alias <I123> = <VOL+>;\n"
    "  alias <I124> = <POWR>;\n"
    "  alias <I125> = <KPEQ>;\n"
    "  alias <I127> = <PAUS>;\n"
    "  alias <I130> = <HNGL>;\n"
    "  alias <I131> = <HJCV>;\n"
    "  alias <I132> = <AE13>;\n"
    "  alias <I133> = <LWIN>;\n"
    "  alias <I134> = <RWIN>;\n"
    "  alias <I135> = <COMP>;\n"
    "  alias <I136> = <STOP>;\n"
    "  alias <I137> = <AGAI>;\n"
    "  alias <I138> = <PROP>;\n"
    "  alias <I139> = <UNDO>;\n"
    "  alias <I140> = <FRNT>;\n"
    "  alias <I141> = <COPY>;\n"
    "  alias <I142> = <OPEN>;\n"
    "  alias <I143> = <PAST>;\n"
    "  alias <I144> = <FIND>;\n"
    "  alias <I145> = <CUT>;\n"
    "  alias <I146> = <HELP>;\n"
    "  alias <I191> = <FK13>;\n"
    "  alias <I192> = <FK14>;\n"
    "  alias <I193> = <FK15>;\n"
    "  alias <I194> = <FK16>;\n"
    "  alias <I195> = <FK17>;\n"
    "  alias <I196> = <FK18>;\n"
    "  alias <I197> = <FK19>;\n"
    "  alias <I198> = <FK20>;\n"
    "  alias <I199> = <FK21>;\n"
    "  alias <I200> = <FK22>;\n"
    "  alias <I201> = <FK23>;\n"
    "  alias <I202> = <FK24>;\n"
    "  alias <MDSW> = <LVL5>;\n"
    "  alias <KPPT> = <I129>;\n"
    "  alias <LatQ> = <AD01>;\n"
    "  alias <LatW> = <AD02>;\n"
    "  alias <LatE> = <AD03>;\n"
    "  alias <LatR> = <AD04>;\n"
    "  alias <LatT> = <AD05>;\n"
    "  alias <LatY> = <AD06>;\n"
    "  alias <LatU> = <AD07>;\n"
    "  alias <LatI> = <AD08>;\n"
    "  alias <LatO> = <AD09>;\n"
    "  alias <LatP> = <AD10>;\n"
    "  alias <LatA> = <AC01>;\n"
    "  alias <LatS> = <AC02>;\n"
    "  alias <LatD> = <AC03>;\n"
    "  alias <LatF> = <AC04>;\n"
    "  alias <LatG> = <AC05>;\n"
    "  alias <LatH> = <AC06>;\n"
    "  alias <LatJ> = <AC07>;\n"
    "  alias <LatK> = <AC08>;\n"
    "  alias <LatL> = <AC09>;\n"
    "  alias <LatZ> = <AB01>;\n"
    "  alias <LatX> = <AB02>;\n"
    "  alias <LatC> = <AB03>;\n"
    "  alias <LatV> = <AB04>;\n"
    "  alias <LatB> = <AB05>;\n"
    "  alias <LatN> = <AB06>;\n"
    "  alias <LatM> = <AB07>;\n"
    "};\n"
    "xkb_types \"lumen\" {\n"
    "  virtual_modifiers NumLock,Alt,LevelThree,LevelFive,Meta,Super,Hyper,ScrollLock;\n"
    "  type \"ONE_LEVEL\" {\n"
    "    modifiers = none;\n"
    "    level_name[1] = \"Any\";\n"
    "  };\n"
    "  type \"TWO_LEVEL\" {\n"
    "    modifiers = Shift;\n"
    "    map[Shift] = 2;\n"
    "    level_name[1] = \"Base\";\n"
    "    level_name[2] = \"Shift\";\n"
    "  };\n"
    "  type \"ALPHABETIC\" {\n"
    "    modifiers = Shift+Lock;\n"
    "    map[Shift] = 2;\n"
    "    map[Lock] = 2;\n"
    "    level_name[1] = \"Base\";\n"
    "    level_name[2] = \"Caps\";\n"
    "  };\n"
    "  type \"KEYPAD\" {\n"
    "    modifiers = Shift+NumLock;\n"
    "    map[NumLock] = 2;\n"
    "    level_name[1] = \"Base\";\n"
    "    level_name[2] = \"Number\";\n"
    "  };\n"
    "  type \"PC_ALT_LEVEL2\" {\n"
    "    modifiers = Alt;\n"
    "    map[Alt] = 2;\n"
    "    level_name[1] = \"Base\";\n"
    "    level_name[2] = \"Alt\";\n"
    "  };\n"
    "  type \"PC_CONTROL_LEVEL2\" {\n"
    "    modifiers = Control;\n"
    "    map[Control] = 2;\n"
    "    level_name[1] = \"Base\";\n"
    "    level_name[2] = \"Control\";\n"
    "  };\n"
    "};\n"
    "xkb_compatibility \"lumen\" {\n"
    "  virtual_modifiers NumLock,Alt,LevelThree,LevelFive,Meta,Super,Hyper,ScrollLock;\n"
    "  interpret.useModMapMods = AnyLevel;\n"
    "  interpret.repeat = False;\n"
    "  interpret Num_Lock+AnyOf(all) {\n"
    "    virtualModifier = NumLock;\n"
    "    action = LockMods(modifiers=NumLock);\n"
    "  };\n"
    "  interpret Scroll_Lock+AnyOf(all) {\n"
    "    virtualModifier = ScrollLock;\n"
    "    action = LockMods(modifiers=modMapMods);\n"
    "  };\n"
    "  interpret Alt_L+AnyOf(all) {\n"
    "    virtualModifier = Alt;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Alt_R+AnyOf(all) {\n"
    "    virtualModifier = Alt;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Meta_L+AnyOf(all) {\n"
    "    virtualModifier = Meta;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Meta_R+AnyOf(all) {\n"
    "    virtualModifier = Meta;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Super_L+AnyOf(all) {\n"
    "    virtualModifier = Super;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Super_R+AnyOf(all) {\n"
    "    virtualModifier = Super;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Hyper_L+AnyOf(all) {\n"
    "    virtualModifier = Hyper;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Hyper_R+AnyOf(all) {\n"
    "    virtualModifier = Hyper;\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  interpret Caps_Lock+AnyOfOrNone(all) {\n"
    "    action = LockMods(modifiers=Lock);\n"
    "  };\n"
    "  interpret Any+Exactly(Lock) {\n"
    "    action = LockMods(modifiers=Lock);\n"
    "  };\n"
    "  interpret Any+AnyOf(all) {\n"
    "    action = SetMods(modifiers=modMapMods,clearLocks);\n"
    "  };\n"
    "  indicator \"Caps Lock\" {\n"
    "    whichModState = locked;\n"
    "    modifiers = Lock;\n"
    "  };\n"
    "  indicator \"Num Lock\" {\n"
    "    whichModState = locked;\n"
    "    modifiers = NumLock;\n"
    "  };\n"
    "  indicator \"Scroll Lock\" {\n"
    "    whichModState = locked;\n"
    "    modifiers = ScrollLock;\n"
    "  };\n"
    "};\n"
    "xkb_symbols \"lumen\" {\n"
    "  name[Group1] = \"English (US)\";\n"
    "  key <ESC> { [ Escape ] };\n"
    "  key <AE01> { [ 1, exclam ] };\n"
    "  key <AE02> { [ 2, at ] };\n"
    "  key <AE03> { [ 3, numbersign ] };\n"
    "  key <AE04> { [ 4, dollar ] };\n"
    "  key <AE05> { [ 5, percent ] };\n"
    "  key <AE06> { [ 6, asciicircum ] };\n"
    "  key <AE07> { [ 7, ampersand ] };\n"
    "  key <AE08> { [ 8, asterisk ] };\n"
    "  key <AE09> { [ 9, parenleft ] };\n"
    "  key <AE10> { [ 0, parenright ] };\n"
    "  key <AE11> { [ minus, underscore ] };\n"
    "  key <AE12> { [ equal, plus ] };\n"
    "  key <BKSP> { [ BackSpace, BackSpace ] };\n"
    "  key <TAB> { [ Tab, ISO_Left_Tab ] };\n"
    "  key <AD01> { [ q, Q ] };\n"
    "  key <AD02> { [ w, W ] };\n"
    "  key <AD03> { [ e, E ] };\n"
    "  key <AD04> { [ r, R ] };\n"
    "  key <AD05> { [ t, T ] };\n"
    "  key <AD06> { [ y, Y ] };\n"
    "  key <AD07> { [ u, U ] };\n"
    "  key <AD08> { [ i, I ] };\n"
    "  key <AD09> { [ o, O ] };\n"
    "  key <AD10> { [ p, P ] };\n"
    "  key <AD11> { [ bracketleft, braceleft ] };\n"
    "  key <AD12> { [ bracketright, braceright ] };\n"
    "  key <RTRN> { [ Return ] };\n"
    "  key <LCTL> { [ Control_L ] };\n"
    "  key <AC01> { [ a, A ] };\n"
    "  key <AC02> { [ s, S ] };\n"
    "  key <AC03> { [ d, D ] };\n"
    "  key <AC04> { [ f, F ] };\n"
    "  key <AC05> { [ g, G ] };\n"
    "  key <AC06> { [ h, H ] };\n"
    "  key <AC07> { [ j, J ] };\n"
    "  key <AC08> { [ k, K ] };\n"
    "  key <AC09> { [ l, L ] };\n"
    "  key <AC10> { [ semicolon, colon ] };\n"
    "  key <AC11> { [ apostrophe, quotedbl ] };\n"
    "  key <TLDE> { [ grave, asciitilde ] };\n"
    "  key <LFSH> { [ Shift_L ] };\n"
    "  key <BKSL> { [ backslash, bar ] };\n"
    "  key <AB01> { [ z, Z ] };\n"
    "  key <AB02> { [ x, X ] };\n"
    "  key <AB03> { [ c, C ] };\n"
    "  key <AB04> { [ v, V ] };\n"
    "  key <AB05> { [ b, B ] };\n"
    "  key <AB06> { [ n, N ] };\n"
    "  key <AB07> { [ m, M ] };\n"
    "  key <AB08> { [ comma, less ] };\n"
    "  key <AB09> { [ period, greater ] };\n"
    "  key <AB10> { [ slash, question ] };\n"
    "  key <RTSH> { [ Shift_R ] };\n"
    "  key <KPMU> { [ KP_Multiply ] };\n"
    "  key <LALT> { [ Alt_L, Meta_L ] };\n"
    "  key <SPCE> { [ space ] };\n"
    "  key <CAPS> { [ Caps_Lock ] };\n"
    "  key <FK01> { [ F1 ] };\n"
    "  key <FK02> { [ F2 ] };\n"
    "  key <FK03> { [ F3 ] };\n"
    "  key <FK04> { [ F4 ] };\n"
    "  key <FK05> { [ F5 ] };\n"
    "  key <FK06> { [ F6 ] };\n"
    "  key <FK07> { [ F7 ] };\n"
    "  key <FK08> { [ F8 ] };\n"
    "  key <FK09> { [ F9 ] };\n"
    "  key <FK10> { [ F10 ] };\n"
    "  key <NMLK> { [ Num_Lock ] };\n"
    "  key <SCLK> { [ Scroll_Lock ] };\n"
    "  key <KP7> { [ KP_Home, KP_7 ] };\n"
    "  key <KP8> { [ KP_Up, KP_8 ] };\n"
    "  key <KP9> { [ KP_Prior, KP_9 ] };\n"
    "  key <KPSU> { [ KP_Subtract ] };\n"
    "  key <KP4> { [ KP_Left, KP_4 ] };\n"
    "  key <KP5> { [ KP_Begin, KP_5 ] };\n"
    "  key <KP6> { [ KP_Right, KP_6 ] };\n"
    "  key <KPAD> { [ KP_Add ] };\n"
    "  key <KP1> { [ KP_End, KP_1 ] };\n"
    "  key <KP2> { [ KP_Down, KP_2 ] };\n"
    "  key <KP3> { [ KP_Next, KP_3 ] };\n"
    "  key <KP0> { [ KP_Insert, KP_0 ] };\n"
    "  key <KPDL> { [ KP_Delete, KP_Decimal ] };\n"
    "  key <LVL3> { [ ISO_Level3_Shift ] };\n"
    "  key <LSGT> { [ less, greater ] };\n"
    "  key <FK11> { [ F11 ] };\n"
    "  key <FK12> { [ F12 ] };\n"
    "  key <KATA> { [ Katakana ] };\n"
    "  key <HIRA> { [ Hiragana ] };\n"
    "  key <HENK> { [ Henkan_Mode ] };\n"
    "  key <HKTG> { [ Hiragana_Katakana ] };\n"
    "  key <MUHE> { [ Muhenkan ] };\n"
    "  key <KPEN> { [ KP_Enter ] };\n"
    "  key <RCTL> { [ Control_R ] };\n"
    "  key <KPDV> { [ KP_Divide ] };\n"
    "  key <PRSC> {\n"
    "    type = \"PC_ALT_LEVEL2\",\n"
    "    symbols[Group1] = [ Print, Sys_Req ]\n"
    "  };\n"
    "  key <RALT> { [ Alt_R, Meta_R ] };\n"
    "  key <LNFD> { [ Linefeed ] };\n"
    "  key <HOME> { [ Home ] };\n"
    "  key <UP> { [ Up ] };\n"
    "  key <PGUP> { [ Prior ] };\n"
    "  key <LEFT> { [ Left ] };\n"
    "  key <RGHT> { [ Right ] };\n"
    "  key <END> { [ End ] };\n"
    "  key <DOWN> { [ Down ] };\n"
    "  key <PGDN> { [ Next ] };\n"
    "  key <INS> { [ Insert ] };\n"
    "  key <DELE> { [ Delete ] };\n"
    "  key <MUTE> { [ XF86AudioMute ] };\n"
    "  key <VOL-> { [ XF86AudioLowerVolume ] };\n"
    "  key <VOL+> { [ XF86AudioRaiseVolume ] };\n"
    "  key <POWR> { [ XF86PowerOff ] };\n"
    "  key <KPEQ> { [ KP_Equal ] };\n"
    "  key <I126> { [ plusminus ] };\n"
    "  key <PAUS> {\n"
    "    type = \"PC_CONTROL_LEVEL2\",\n"
    "    symbols[Group1] = [ Pause, Break ]\n"
    "  };\n"
    "  key <I128> { [ XF86LaunchA ] };\n"
    "  key <I129> { [ KP_Decimal, KP_Decimal ] };\n"
    "  key <HNGL> { [ Hangul ] };\n"
    "  key <HJCV> { [ Hangul_Hanja ] };\n"
    "  key <LWIN> { [ Super_L ] };\n"
    "  key <RWIN> { [ Super_R ] };\n"
    "  key <COMP> { [ Menu ] };\n"
    "  key <STOP> { [ Cancel ] };\n"
    "  key <AGAI> { [ Redo ] };\n"
    "  key <PROP> { [ SunProps ] };\n"
    "  key <UNDO> { [ Undo ] };\n"
    "  key <FRNT> { [ SunFront ] };\n"
    "  key <COPY> { [ XF86Copy ] };\n"
    "  key <OPEN> { [ XF86Open ] };\n"
    "  key <PAST> { [ XF86Paste ] };\n"
    "  key <FIND> { [ Find ] };\n"
    "  key <CUT> { [ XF86Cut ] };\n"
    "  key <HELP> { [ Help ] };\n"
    "  key <I147> { [ XF86MenuKB ] };\n"
    "  key <I148> { [ XF86Calculator ] };\n"
    "  key <I150> { [ XF86Sleep ] };\n"
    "  key <I151> { [ XF86WakeUp ] };\n"
    "  key <I152> { [ XF86Explorer ] };\n"
    "  key <I153> { [ XF86Send ] };\n"
    "  key <I155> { [ XF86Xfer ] };\n"
    "  key <I156> { [ XF86Launch1 ] };\n"
    "  key <I157> { [ XF86Launch2 ] };\n"
    "  key <I158> { [ XF86WWW ] };\n"
    "  key <I159> { [ XF86DOS ] };\n"
    "  key <I160> { [ XF86ScreenSaver ] };\n"
    "  key <I161> { [ XF86RotateWindows ] };\n"
    "  key <I162> { [ XF86TaskPane ] };\n"
    "  key <I163> { [ XF86Mail ] };\n"
    "  key <I164> { [ XF86Favorites ] };\n"
    "  key <I165> { [ XF86MyComputer ] };\n"
    "  key <I166> { [ XF86Back ] };\n"
    "  key <I167> { [ XF86Forward ] };\n"
    "  key <I169> { [ XF86Eject ] };\n"
    "  key <I170> { [ XF86Eject ] };\n"
    "  key <I171> { [ XF86AudioNext ] };\n"
    "  key <I172> { [ XF86AudioPlay, XF86AudioPause ] };\n"
    "  key <I173> { [ XF86AudioPrev ] };\n"
    "  key <I174> { [ XF86AudioStop, XF86Eject ] };\n"
    "  key <I175> { [ XF86AudioRecord ] };\n"
    "  key <I176> { [ XF86AudioRewind ] };\n"
    "  key <I177> { [ XF86Phone ] };\n"
    "  key <I179> { [ XF86Tools ] };\n"
    "  key <I180> { [ XF86HomePage ] };\n"
    "  key <I181> { [ XF86Reload ] };\n"
    "  key <I182> { [ XF86Close ] };\n"
    "  key <I185> { [ XF86ScrollUp ] };\n"
    "  key <I186> { [ XF86ScrollDown ] };\n"
    "  key <I187> { [ parenleft ] };\n"
    "  key <I188> { [ parenright ] };\n"
    "  key <I189> { [ XF86New ] };\n"
    "  key <I190> { [ Redo ] };\n"
    "  key <FK13> { [ XF86Tools ] };\n"
    "  key <FK14> { [ XF86Launch5 ] };\n"
    "  key <FK15> { [ XF86Launch6 ] };\n"
    "  key <FK16> { [ XF86Launch7 ] };\n"
    "  key <FK17> { [ XF86Launch8 ] };\n"
    "  key <FK18> { [ XF86Launch9 ] };\n"
    "  key <FK20> { [ XF86AudioMicMute ] };\n"
    "  key <FK21> { [ XF86TouchpadToggle ] };\n"
    "  key <FK22> { [ XF86TouchpadOn ] };\n"
    "  key <FK23> { [ XF86TouchpadOff ] };\n"
    "  key <LVL5> { [ ISO_Level5_Shift ] };\n"
    "  key <ALT> { [ NoSymbol, Alt_L ] };\n"
    "  key <META> { [ NoSymbol, Meta_L ] };\n"
    "  key <SUPR> { [ NoSymbol, Super_L ] };\n"
    "  key <HYPR> { [ NoSymbol, Hyper_L ] };\n"
    "  key <I208> { [ XF86AudioPlay ] };\n"
    "  key <I209> { [ XF86AudioPause ] };\n"
    "  key <I210> { [ XF86Launch3 ] };\n"
    "  key <I211> { [ XF86Launch4 ] };\n"
    "  key <I212> { [ XF86LaunchB ] };\n"
    "  key <I213> { [ XF86Suspend ] };\n"
    "  key <I214> { [ XF86Close ] };\n"
    "  key <I215> { [ XF86AudioPlay ] };\n"
    "  key <I216> { [ XF86AudioForward ] };\n"
    "  key <I218> { [ Print ] };\n"
    "  key <I220> { [ XF86WebCam ] };\n"
    "  key <I221> { [ XF86AudioPreset ] };\n"
    "  key <I223> { [ XF86Mail ] };\n"
    "  key <I224> { [ XF86Messenger ] };\n"
    "  key <I225> { [ XF86Search ] };\n"
    "  key <I226> { [ XF86Go ] };\n"
    "  key <I227> { [ XF86Finance ] };\n"
    "  key <I228> { [ XF86Game ] };\n"
    "  key <I229> { [ XF86Shop ] };\n"
    "  key <I231> { [ Cancel ] };\n"
    "  key <I232> { [ XF86MonBrightnessDown ] };\n"
    "  key <I233> { [ XF86MonBrightnessUp ] };\n"
    "  key <I234> { [ XF86AudioMedia ] };\n"
    "  key <I235> { [ XF86Display ] };\n"
    "  key <I236> { [ XF86KbdLightOnOff ] };\n"
    "  key <I237> { [ XF86KbdBrightnessDown ] };\n"
    "  key <I238> { [ XF86KbdBrightnessUp ] };\n"
    "  key <I239> { [ XF86Send ] };\n"
    "  key <I240> { [ XF86Reply ] };\n"
    "  key <I241> { [ XF86MailForward ] };\n"
    "  key <I242> { [ XF86Save ] };\n"
    "  key <I243> { [ XF86Documents ] };\n"
    "  key <I244> { [ XF86Battery ] };\n"
    "  key <I245> { [ XF86Bluetooth ] };\n"
    "  key <I246> { [ XF86WLAN ] };\n"
    "  key <I247> { [ XF86UWB ] };\n"
    "  key <I249> { [ XF86Next_VMode ] };\n"
    "  key <I250> { [ XF86Prev_VMode ] };\n"
    "  key <I251> { [ XF86MonBrightnessCycle ] };\n"
    "  key <I252> { [ XF86BrightnessAuto ] };\n"
    "  key <I253> { [ XF86DisplayOff ] };\n"
    "  key <I254> { [ XF86WWAN ] };\n"
    "  key <I255> { [ XF86RFKill ] };\n"
    "  modifier_map Shift { <LFSH>, <RTSH> };\n"
    "  modifier_map Lock { <CAPS> };\n"
    "  modifier_map Control { <LCTL>, <RCTL> };\n"
    "  modifier_map Mod1 { <LALT>, <RALT> };\n"
    "  modifier_map Mod2 { <NMLK> };\n"
    "  modifier_map Mod4 { <LWIN>, <RWIN> };\n"
    "};\n"
    "};\n"
    ;

/* Weak so that a dedicated keymap.c (shared with clients) may take over. */
__attribute__((weak)) const char *lm_keymap_us_text(void)
{
    return lumen_keymap_text;
}

/* ---- state ---------------------------------------------------------------------------- */

struct seat_binding {                   /* a bound wl_seat */
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_pointer {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_keyboard {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_touch {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct data_source {
    struct lm_list link;                /* in seat->sources */
    struct lm_seat *seat;
    struct lm_resource *res;
    char *mimes[MAX_MIMES];
    int n_mimes;
    uint32_t dnd_actions;
    struct lm_list offers;              /* struct data_offer.src_link */
};

struct data_offer {
    struct lm_list link;                /* in seat->offers */
    struct lm_list src_link;            /* in source->offers (detached when source gone) */
    struct lm_seat *seat;
    struct data_source *source;         /* NULL once the source is destroyed */
    struct lm_resource *res;
};

struct data_device {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_client {                    /* per client: purge serials when it leaves */
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_client *client;
    struct lm_listener destroy;
};

struct serial_rec {
    uint32_t serial;
    struct lm_client *client;
};

struct touch_point {
    bool active;
    bool via_pointer;                   /* decoration touch: emulated through the pointer path */
    int32_t id;
    struct lm_surface *surface;
    double ox, oy;                      /* global origin of the surface */
    struct lm_listener destroy;
    struct lm_seat *seat;
};

struct lm_seat {
    struct lm_server *srv;
    struct lm_global *seat_global;
    struct lm_global *ddm_global;
    bool dying;

    struct lm_list bindings;            /* struct seat_binding */
    struct lm_list pointers;            /* struct seat_pointer */
    struct lm_list keyboards;           /* struct seat_keyboard */
    struct lm_list touches;             /* struct seat_touch */
    struct lm_list devices;             /* struct data_device */
    struct lm_list sources;             /* struct data_source */
    struct lm_list offers;              /* struct data_offer */
    struct lm_list clients;             /* struct seat_client */

    /* pointer */
    double px, py;
    bool pos_valid;
    struct lm_surface *pfocus;
    struct lm_listener pfocus_destroy;
    double pox, poy;                    /* global origin of the focus surface */
    uint32_t enter_serial;
    struct lm_pointer_grab *grab;
    uint32_t fwd_buttons[MAX_BUTTONS];  /* presses forwarded to a client, not yet released */
    int n_fwd;
    struct lm_idle *refocus_idle;

    /* cursor */
    enum lm_cursor_shape override;
    struct lm_surface *cursor_surface;
    struct lm_listener cursor_destroy;
    struct lm_client *cursor_client;
    int32_t cursor_hot_x, cursor_hot_y;
    bool cursor_hidden;

    /* keyboard */
    struct lm_surface *kfocus;
    struct lm_listener kfocus_destroy;
    uint32_t keys_down[MAX_KEYS];       /* evdev codes of forwarded, still held keys */
    int n_keys;
    uint32_t swallowed[MAX_KEYS];       /* codes whose press the wm consumed */
    int n_swallowed;
    uint32_t mods_dep, mods_lock;
    int keymap_fd;
    uint32_t keymap_size;

    /* serials */
    struct serial_rec serials[MAX_SERIALS];
    int serial_next;

    /* touch */
    bool has_touch;
    struct touch_point points[MAX_TOUCH];

    /* clipboard */
    struct data_source *selection;
};

/* ---- small helpers -------------------------------------------------------------------- */

static uint32_t next_serial(struct lm_seat *seat)
{
    return lm_server_next_serial(seat->srv);
}

static void cursor_changed(struct lm_seat *seat)
{
    if (seat->srv->render)
        lm_render_cursor_changed(seat->srv->render);
}

static struct lm_client *surface_client(const struct lm_surface *s)
{
    return s && s->resource ? s->resource->client : NULL;
}

static bool code_find(const uint32_t *arr, int n, uint32_t code)
{
    for (int i = 0; i < n; i++)
        if (arr[i] == code)
            return true;
    return false;
}

static bool code_add(uint32_t *arr, int *n, int max, uint32_t code)
{
    if (code_find(arr, *n, code))
        return true;
    if (*n >= max)
        return false;
    arr[(*n)++] = code;
    return true;
}

static bool code_remove(uint32_t *arr, int *n, uint32_t code)
{
    for (int i = 0; i < *n; i++) {
        if (arr[i] == code) {
            memmove(&arr[i], &arr[i + 1], sizeof(arr[0]) * (size_t)(*n - i - 1));
            (*n)--;
            return true;
        }
    }
    return false;
}

/* Destroy every resource of a per-seat list (used when the seat goes away first). */
#define DESTROY_ALL(type, head) \
    do { \
        type *x_, *t_; \
        lm_list_for_each_safe(x_, t_, head, link) \
            lm_resource_destroy(x_->res); \
    } while (0)

/* Every pointer/keyboard/... object belonging to `cl`. */
#define FOR_CLIENT_OBJECTS(pos, head, cl) \
    lm_list_for_each(pos, head, link) if ((pos)->res->client == (cl))

static void ensure_pointer_pos(struct lm_seat *seat)
{
    if (seat->pos_valid)
        return;
    struct lm_rect b;
    if (lm_output_layout_bounds(seat->srv, &b)) {
        seat->px = b.x + b.w / 2.0;
        seat->py = b.y + b.h / 2.0;
        seat->pos_valid = true;
    }
}

/* ---- serials ---------------------------------------------------------------------------- */

static void seat_client_gone(struct lm_listener *l, void *data)
{
    (void)data;
    struct seat_client *sc = lm_container_of(l, struct seat_client, destroy);
    struct lm_seat *seat = sc->seat;
    lm_listener_remove(l);
    for (int i = 0; i < MAX_SERIALS; i++)
        if (seat->serials[i].client == sc->client)
            seat->serials[i].client = NULL;
    if (seat->cursor_client == sc->client)
        seat->cursor_client = NULL;
    lm_list_remove(&sc->link);
    free(sc);
}

static void ensure_seat_client(struct lm_seat *seat, struct lm_client *client)
{
    struct seat_client *sc;
    lm_list_for_each(sc, &seat->clients, link)
        if (sc->client == client)
            return;
    sc = calloc(1, sizeof(*sc));
    if (!sc)
        return;
    sc->seat = seat;
    sc->client = client;
    sc->destroy.notify = seat_client_gone;
    lm_signal_add(&client->destroy_signal, &sc->destroy);
    lm_list_append(&seat->clients, &sc->link);
}

static void serial_record(struct lm_seat *seat, struct lm_client *client, uint32_t serial)
{
    if (!client)
        return;
    ensure_seat_client(seat, client);
    seat->serials[seat->serial_next].serial = serial;
    seat->serials[seat->serial_next].client = client;
    seat->serial_next = (seat->serial_next + 1) % MAX_SERIALS;
}

bool lm_seat_serial_valid(const struct lm_seat *seat, struct lm_client *client, uint32_t serial)
{
    if (!seat || !client || serial == 0)
        return false;
    for (int i = 0; i < MAX_SERIALS; i++)
        if (seat->serials[i].serial == serial && seat->serials[i].client == client)
            return true;
    return false;
}

/* ---- cursor ----------------------------------------------------------------------------------- */

static void cursor_role_commit(struct lm_surface *s)
{
    struct lm_seat *seat = s->role_data;
    if (seat)
        cursor_changed(seat);
}

static void cursor_role_damage(struct lm_surface *s, const struct lm_rect *rect)
{
    (void)rect;
    struct lm_seat *seat = s->role_data;
    if (seat)
        cursor_changed(seat);
}

static const struct lm_surface_role cursor_role = {
    .name = "cursor",
    .commit = cursor_role_commit,
    .damage = cursor_role_damage,
};

static void cursor_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, cursor_destroy);
    seat->cursor_surface = NULL;
    seat->cursor_client = NULL;
    lm_listener_remove(l);
    cursor_changed(seat);
}

/* Forget the client-provided cursor surface (and release its role). */
static void cursor_drop(struct lm_seat *seat)
{
    struct lm_surface *s = seat->cursor_surface;
    seat->cursor_surface = NULL;
    seat->cursor_client = NULL;
    seat->cursor_hidden = false;
    lm_listener_remove(&seat->cursor_destroy);
    if (s && s->role == &cursor_role && s->role_data)
        lm_surface_clear_role(s);
}

void lm_seat_get_cursor(const struct lm_seat *seat, struct lm_cursor *out)
{
    memset(out, 0, sizeof(*out));
    out->shape = LM_CURSOR_DEFAULT;
    if (!seat)
        return;
    if (seat->override != LM_CURSOR_NONE) {
        out->shape = seat->override;
        return;
    }
    if (seat->pfocus && seat->cursor_client == surface_client(seat->pfocus)) {
        if (seat->cursor_surface && seat->cursor_surface->has_content) {
            out->shape = LM_CURSOR_SURFACE;
            out->surface = seat->cursor_surface;
            out->hot_x = seat->cursor_hot_x;
            out->hot_y = seat->cursor_hot_y;
        } else if (seat->cursor_hidden || seat->cursor_surface) {
            out->shape = LM_CURSOR_HIDDEN;
        }
    }
}

void lm_seat_set_cursor_override(struct lm_seat *seat, enum lm_cursor_shape shape)
{
    if (!seat || seat->override == shape)
        return;
    seat->override = shape;
    cursor_changed(seat);
}

void lm_seat_send_frame_done(struct lm_seat *seat, uint32_t time_ms)
{
    if (seat && seat->cursor_surface)
        lm_surface_send_frame_done(seat->cursor_surface, time_ms);
}

/* ---- wl_pointer events ---------------------------------------------------------------------------- */

static void pointer_frame(struct lm_seat *seat, struct lm_client *cl)
{
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_FRAME);
}

static void pfocus_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, pfocus_destroy);
    lm_listener_remove(l);
    seat->pfocus = NULL;
    seat->n_fwd = 0;
    cursor_changed(seat);
    if (seat->refocus_idle && !seat->dying)
        lm_idle_schedule(seat->refocus_idle);
}

void lm_seat_pointer_set_focus(struct lm_seat *seat, struct lm_surface *s, double sx, double sy)
{
    if (!seat)
        return;
    if (s && !s->resource)
        s = NULL;
    if (s == seat->pfocus) {
        if (s) {
            seat->pox = seat->px - sx;
            seat->poy = seat->py - sy;
        }
        return;
    }

    if (seat->pfocus) {
        struct lm_surface *old = seat->pfocus;
        struct lm_client *cl = surface_client(old);
        uint32_t serial = next_serial(seat);
        struct seat_pointer *p;
        FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
            lm_resource_post(p->res, LM_WL_POINTER_EVT_LEAVE, serial, old->resource);
        pointer_frame(seat, cl);
        lm_listener_remove(&seat->pfocus_destroy);
        seat->pfocus = NULL;
    }

    if (s) {
        seat->pfocus = s;
        seat->pfocus_destroy.notify = pfocus_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &seat->pfocus_destroy);
        seat->pox = seat->px - sx;
        seat->poy = seat->py - sy;
        seat->enter_serial = next_serial(seat);
        struct lm_client *cl = surface_client(s);
        struct seat_pointer *p;
        FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
            lm_resource_post(p->res, LM_WL_POINTER_EVT_ENTER, seat->enter_serial, s->resource,
                             lm_fixed_from_double(sx), lm_fixed_from_double(sy));
        pointer_frame(seat, cl);
    }

    /* The cursor image belongs to the client that set it. */
    if (seat->cursor_client && seat->cursor_client != surface_client(s))
        cursor_drop(seat);
    cursor_changed(seat);
}

void lm_seat_pointer_send_motion(struct lm_seat *seat, uint32_t time_ms)
{
    if (!seat || !seat->pfocus)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    lm_fixed_t fx = lm_fixed_from_double(seat->px - seat->pox);
    lm_fixed_t fy = lm_fixed_from_double(seat->py - seat->poy);
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_MOTION, time_ms, fx, fy);
    pointer_frame(seat, cl);
}

void lm_seat_pointer_send_button(struct lm_seat *seat, uint32_t time_ms, uint32_t button,
                                 bool pressed)
{
    if (!seat || !seat->pfocus)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    uint32_t serial = next_serial(seat);
    if (pressed)
        serial_record(seat, cl, serial);
    uint32_t state = pressed ? LM_POINTER_BUTTON_PRESSED : LM_POINTER_BUTTON_RELEASED;
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_BUTTON, serial, time_ms, button, state);
    pointer_frame(seat, cl);
}

void lm_seat_pointer_send_axis(struct lm_seat *seat, uint32_t time_ms, int orientation,
                               double value, int32_t discrete)
{
    if (!seat || !seat->pfocus)
        return;
    if (orientation != LM_POINTER_AXIS_VERTICAL && orientation != LM_POINTER_AXIS_HORIZONTAL)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    uint32_t axis = (uint32_t)orientation;
    lm_fixed_t fv = lm_fixed_from_double(value);
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl) {
        if (discrete != 0) {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_WHEEL);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_DISCRETE, axis, discrete);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS, time_ms, axis, fv);
        } else if (value == 0.0) {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_FINGER);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_STOP, time_ms, axis);
        } else {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_FINGER);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS, time_ms, axis, fv);
        }
    }
    pointer_frame(seat, cl);
}

void lm_seat_pointer_position(const struct lm_seat *seat, double *x, double *y)
{
    double px = 0, py = 0;
    if (seat) {
        px = seat->px;
        py = seat->py;
        if (!seat->pos_valid) {
            struct lm_rect b;
            if (lm_output_layout_bounds(seat->srv, &b)) {
                px = b.x + b.w / 2.0;
                py = b.y + b.h / 2.0;
            }
        }
    }
    if (x)
        *x = px;
    if (y)
        *y = py;
}

struct lm_surface *lm_seat_pointer_focus(const struct lm_seat *seat)
{
    return seat ? seat->pfocus : NULL;
}

/* ---- pointer routing ---------------------------------------------------------------------------- */

static void pick_at(struct lm_seat *seat, struct lm_pick *pick)
{
    memset(pick, 0, sizeof(*pick));
    if (seat->srv->wm)
        lm_wm_pick(seat->srv->wm, seat->px, seat->py, pick);
}

static bool pick_is_surface(const struct lm_pick *pick)
{
    return pick->kind == LM_PICK_SURFACE && pick->surface && pick->surface->resource;
}

/* Set the pointer focus from a pick. Returns true if the focus changed. */
static bool focus_from_pick(struct lm_seat *seat, const struct lm_pick *pick)
{
    struct lm_surface *old = seat->pfocus;
    if (pick_is_surface(pick))
        lm_seat_pointer_set_focus(seat, pick->surface, pick->sx, pick->sy);
    else
        lm_seat_pointer_set_focus(seat, NULL, 0, 0);
    return old != seat->pfocus;
}

static void pointer_refocus(struct lm_seat *seat, uint32_t time_ms)
{
    if (seat->grab || seat->srv->wm == NULL)
        return;
    struct lm_pick pick;
    pick_at(seat, &pick);
    if (focus_from_pick(seat, &pick) && seat->pfocus)
        lm_seat_pointer_send_motion(seat, time_ms);
}

static void refocus_idle_cb(void *data)
{
    struct lm_seat *seat = data;
    if (!seat->dying && seat->n_fwd == 0)
        pointer_refocus(seat, lm_now_msec32());
}

/* The pointer position changed: route the motion. */
static void pointer_moved(struct lm_seat *seat, uint32_t time_ms)
{
    cursor_changed(seat);
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->motion)
            g->ops->motion(g, time_ms, seat->px, seat->py);
        return;
    }
    struct lm_pick pick;
    pick_at(seat, &pick);
    if (seat->srv->wm)
        lm_wm_pointer_motion(seat->srv->wm, &pick, seat->px, seat->py);
    if (seat->grab)             /* the wm may have started one */
        return;

    if (seat->n_fwd > 0) {
        /* Implicit grab: while a forwarded button is held the focus sticks. */
        if (seat->pfocus)
            lm_seat_pointer_send_motion(seat, time_ms);
        return;
    }
    if (pick_is_surface(&pick) && pick.surface == seat->pfocus) {
        seat->pox = seat->px - pick.sx;
        seat->poy = seat->py - pick.sy;
        lm_seat_pointer_send_motion(seat, time_ms);
    } else {
        focus_from_pick(seat, &pick);
        if (seat->pfocus)
            lm_seat_pointer_send_motion(seat, time_ms);
    }
}

static void pointer_set_position(struct lm_seat *seat, double x, double y, uint32_t time_ms)
{
    lm_output_clamp_point(seat->srv, &x, &y);
    seat->px = x;
    seat->py = y;
    seat->pos_valid = true;
    pointer_moved(seat, time_ms);
}

void lm_seat_pointer_warp(struct lm_seat *seat, double x, double y)
{
    if (!seat)
        return;
    pointer_set_position(seat, x, y, lm_now_msec32());
}

static void pointer_button(struct lm_seat *seat, uint32_t time_ms, uint32_t button, bool pressed)
{
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->button)
            g->ops->button(g, time_ms, button, pressed);
        return;
    }
    struct lm_wm *wm = seat->srv->wm;
    struct lm_pick pick;
    if (pressed) {
        if (seat->n_fwd == 0)
            pointer_refocus(seat, time_ms);
        pick_at(seat, &pick);
        bool consumed = wm ? lm_wm_pointer_button(wm, &pick, time_ms, button, true) : false;
        if (seat->grab || consumed)
            return;
        if (seat->pfocus && code_add(seat->fwd_buttons, &seat->n_fwd, MAX_BUTTONS, button))
            lm_seat_pointer_send_button(seat, time_ms, button, true);
    } else {
        pick_at(seat, &pick);
        bool was_fwd = code_remove(seat->fwd_buttons, &seat->n_fwd, button);
        if (wm)
            lm_wm_pointer_button(wm, &pick, time_ms, button, false);
        if (seat->grab)
            return;
        if (was_fwd) {
            if (seat->pfocus)
                lm_seat_pointer_send_button(seat, time_ms, button, false);
            if (seat->n_fwd == 0)
                pointer_refocus(seat, time_ms);
        }
    }
}

static void pointer_axis(struct lm_seat *seat, uint32_t time_ms, int orientation, double value,
                         int32_t discrete)
{
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->axis)
            g->ops->axis(g, time_ms, orientation, value, discrete);
        return;
    }
    if (seat->n_fwd == 0)
        pointer_refocus(seat, time_ms);
    lm_seat_pointer_send_axis(seat, time_ms, orientation, value, discrete);
}

/* ---- pointer grabs ------------------------------------------------------------------------------- */

int lm_seat_pointer_grab(struct lm_seat *seat, struct lm_pointer_grab *grab)
{
    if (!seat || !grab)
        return -1;
    struct lm_pointer_grab *old = seat->grab;
    if (old == grab)
        return 0;
    seat->grab = NULL;
    if (old && old->ops && old->ops->cancel)
        old->ops->cancel(old);
    seat->grab = grab;
    seat->n_fwd = 0;                    /* the clients lose their implicit grab */
    lm_seat_pointer_set_focus(seat, NULL, 0, 0);
    return 0;
}

void lm_seat_pointer_ungrab(struct lm_seat *seat, struct lm_pointer_grab *grab)
{
    if (!seat || !grab || seat->grab != grab)
        return;
    seat->grab = NULL;
    if (!seat->dying)
        pointer_refocus(seat, lm_now_msec32());
}

bool lm_seat_pointer_has_grab(const struct lm_seat *seat)
{
    return seat && seat->grab != NULL;
}

/* ---- keyboard ---------------------------------------------------------------------------------------- */

static void kfocus_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, kfocus_destroy);
    lm_listener_remove(l);
    seat->kfocus = NULL;
}

static void keyboard_send_enter(struct lm_seat *seat, struct seat_keyboard *k, uint32_t serial)
{
    struct lm_array keys = { (uint32_t)seat->n_keys * (uint32_t)sizeof(uint32_t), seat->keys_down };
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_ENTER, serial, seat->kfocus->resource, &keys);
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_MODIFIERS, serial, seat->mods_dep, 0u,
                     seat->mods_lock, 0u);
}

struct lm_surface *lm_seat_keyboard_focus(const struct lm_seat *seat)
{
    return seat ? seat->kfocus : NULL;
}

static void selection_update_focus(struct lm_seat *seat);

void lm_seat_set_keyboard_focus(struct lm_seat *seat, struct lm_surface *s)
{
    if (!seat)
        return;
    if (s && !s->resource)
        s = NULL;
    if (s == seat->kfocus)
        return;

    if (seat->kfocus) {
        struct lm_surface *old = seat->kfocus;
        struct lm_client *cl = surface_client(old);
        uint32_t serial = next_serial(seat);
        struct seat_keyboard *k;
        FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
            lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_LEAVE, serial, old->resource);
        lm_listener_remove(&seat->kfocus_destroy);
        seat->kfocus = NULL;
    }
    if (s) {
        seat->kfocus = s;
        seat->kfocus_destroy.notify = kfocus_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &seat->kfocus_destroy);
        uint32_t serial = next_serial(seat);
        struct lm_client *cl = surface_client(s);
        struct seat_keyboard *k;
        FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
            keyboard_send_enter(seat, k, serial);
    }
    selection_update_focus(seat);
}

static void keyboard_send_modifiers(struct lm_seat *seat)
{
    if (!seat->kfocus)
        return;
    struct lm_client *cl = surface_client(seat->kfocus);
    uint32_t serial = next_serial(seat);
    struct seat_keyboard *k;
    FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
        lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_MODIFIERS, serial, seat->mods_dep, 0u,
                         seat->mods_lock, 0u);
}

static void handle_key(struct lm_seat *seat, const struct lm_input_event *ev)
{
    uint32_t code = ev->u.key.code;
    bool pressed = ev->u.key.pressed;
    uint32_t dep = ev->u.key.mods_depressed;
    uint32_t lock = ev->u.key.mods_locked;
    uint32_t time_ms = ev->time_ms;

    /* Shortcuts only look at depressed modifiers (a locked NumLock must not break them). */
    bool consumed = seat->srv->wm
        ? lm_wm_key(seat->srv->wm, time_ms, code, pressed, ev->u.key.repeat, dep)
        : false;

    if (ev->u.key.repeat) {
        /* Clients repeat on their own; repeats exist for compositor shortcuts only. */
        return;
    }

    if (pressed) {
        if (consumed) {
            code_add(seat->swallowed, &seat->n_swallowed, MAX_KEYS, code);
        } else {
            code_add(seat->keys_down, &seat->n_keys, MAX_KEYS, code);
            if (seat->kfocus) {
                struct lm_client *cl = surface_client(seat->kfocus);
                uint32_t serial = next_serial(seat);
                serial_record(seat, cl, serial);
                struct seat_keyboard *k;
                FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
                    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEY, serial, time_ms, code,
                                     (uint32_t)LM_KEYBOARD_KEY_PRESSED);
            }
        }
    } else {
        if (code_remove(seat->swallowed, &seat->n_swallowed, code)) {
            /* the matching press never reached a client */
        } else if (code_remove(seat->keys_down, &seat->n_keys, code) && seat->kfocus) {
            struct lm_client *cl = surface_client(seat->kfocus);
            uint32_t serial = next_serial(seat);
            struct seat_keyboard *k;
            FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
                lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEY, serial, time_ms, code,
                                 (uint32_t)LM_KEYBOARD_KEY_RELEASED);
        }
    }

    if (dep != seat->mods_dep || lock != seat->mods_lock) {
        seat->mods_dep = dep;
        seat->mods_lock = lock;
        keyboard_send_modifiers(seat);
    }
}

/* wl_keyboard keymap file: a sealed memfd holding the keymap text. */
static int keymap_make_fd(uint32_t *size_out)
{
    const char *text = lumen_keymap_text;
    size_t len = strlen(text) + 1;
    int fd = memfd_create("lumen-xkb-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    bool sealable = fd >= 0;
    if (fd < 0) {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!dir || !*dir)
            dir = "/tmp";
        char path[512];
        if (snprintf(path, sizeof(path), "%s/lumen-keymap-XXXXXX", dir) >= (int)sizeof(path))
            return -1;
        fd = mkostemp(path, O_CLOEXEC);
        if (fd < 0)
            return -1;
        unlink(path);
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    if (sealable)
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    *size_out = (uint32_t)len;
    return fd;
}

/* ---- wl_pointer / wl_keyboard / wl_touch objects ------------------------------------------------------------ */

static void pointer_set_cursor(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_pointer *p = res->data;
    struct lm_seat *seat = p->seat;
    uint32_t serial = args[0].u;
    struct lm_resource *sres = args[1].r;
    int32_t hx = args[2].i, hy = args[3].i;

    struct lm_surface *s = sres ? lm_surface_from_resource(sres) : NULL;
    if (sres && !s)
        return;
    /* Only the client with pointer focus, answering the latest enter, may set the cursor. */
    if (!seat->pfocus || surface_client(seat->pfocus) != res->client)
        return;
    if (serial != seat->enter_serial)
        return;

    if (!s) {
        cursor_drop(seat);
        seat->cursor_hidden = true;
        seat->cursor_client = res->client;
        cursor_changed(seat);
        return;
    }
    if (s == seat->cursor_surface) {
        seat->cursor_hot_x = hx;
        seat->cursor_hot_y = hy;
        cursor_changed(seat);
        return;
    }
    if (s->role && s->role != &cursor_role) {
        lm_resource_post_error(res, LM_WL_POINTER_ERR_ROLE,
                               "wl_surface@%u already has a different role", sres->id);
        return;
    }
    cursor_drop(seat);
    if (lm_surface_set_role(s, &cursor_role, seat) < 0) {
        lm_resource_post_error(res, LM_WL_POINTER_ERR_ROLE,
                               "wl_surface@%u cannot become a cursor surface", sres->id);
        return;
    }
    seat->cursor_surface = s;
    seat->cursor_client = res->client;
    seat->cursor_hidden = false;
    seat->cursor_hot_x = hx;
    seat->cursor_hot_y = hy;
    seat->cursor_destroy.notify = cursor_surface_destroyed;
    lm_signal_add(&s->destroy_signal, &seat->cursor_destroy);
    cursor_changed(seat);
}

static void generic_release(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn pointer_handlers[] = {
    [LM_WL_POINTER_REQ_SET_CURSOR] = pointer_set_cursor,
    [LM_WL_POINTER_REQ_RELEASE] = generic_release,
};

static const lm_request_fn keyboard_handlers[] = {
    [LM_WL_KEYBOARD_REQ_RELEASE] = generic_release,
};

static const lm_request_fn touch_handlers[] = {
    [LM_WL_TOUCH_REQ_RELEASE] = generic_release,
};

static void pointer_res_destroy(struct lm_resource *res)
{
    struct seat_pointer *p = res->data;
    lm_list_remove(&p->link);
    free(p);
}

static void keyboard_res_destroy(struct lm_resource *res)
{
    struct seat_keyboard *k = res->data;
    lm_list_remove(&k->link);
    free(k);
}

static void touch_res_destroy(struct lm_resource *res)
{
    struct seat_touch *t = res->data;
    lm_list_remove(&t->link);
    free(t);
}

static void seat_get_pointer(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_pointer *p = calloc(1, sizeof(*p));
    if (!p) {
        lm_resource_post_no_memory(res);
        return;
    }
    p->seat = seat;
    p->res = lm_resource_create(res->client, args[0].n, &lm_wl_pointer_interface, res->version,
                                pointer_handlers, p, pointer_res_destroy);
    if (!p->res) {
        free(p);
        return;
    }
    lm_list_append(&seat->pointers, &p->link);
    if (seat->pfocus && surface_client(seat->pfocus) == res->client) {
        lm_resource_post(p->res, LM_WL_POINTER_EVT_ENTER, seat->enter_serial,
                         seat->pfocus->resource,
                         lm_fixed_from_double(seat->px - seat->pox),
                         lm_fixed_from_double(seat->py - seat->poy));
        lm_resource_post(p->res, LM_WL_POINTER_EVT_FRAME);
    }
}

/* The keymap file could not be created: tell the client there is none. */
static void keyboard_send_no_keymap(struct seat_keyboard *k)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEYMAP, (uint32_t)LM_KEYBOARD_KEYMAP_NONE, fd, 0u);
    close(fd);
}

static void seat_get_keyboard(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_keyboard *k = calloc(1, sizeof(*k));
    if (!k) {
        lm_resource_post_no_memory(res);
        return;
    }
    k->seat = seat;
    k->res = lm_resource_create(res->client, args[0].n, &lm_wl_keyboard_interface, res->version,
                                keyboard_handlers, k, keyboard_res_destroy);
    if (!k->res) {
        free(k);
        return;
    }
    lm_list_append(&seat->keyboards, &k->link);

    if (seat->keymap_fd >= 0)
        lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEYMAP,
                         (uint32_t)LM_KEYBOARD_KEYMAP_XKB_V1, seat->keymap_fd,
                         seat->keymap_size);
    else
        keyboard_send_no_keymap(k);
    int32_t rate = seat->srv->config.repeat_rate;
    int32_t delay = seat->srv->config.repeat_delay_ms;
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_REPEAT_INFO, rate < 0 ? 0 : rate,
                     delay < 0 ? 0 : delay);
    if (seat->kfocus && surface_client(seat->kfocus) == res->client)
        keyboard_send_enter(seat, k, next_serial(seat));
}

static void seat_get_touch(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_touch *t = calloc(1, sizeof(*t));
    if (!t) {
        lm_resource_post_no_memory(res);
        return;
    }
    t->seat = seat;
    t->res = lm_resource_create(res->client, args[0].n, &lm_wl_touch_interface, res->version,
                                touch_handlers, t, touch_res_destroy);
    if (!t->res) {
        free(t);
        return;
    }
    lm_list_append(&seat->touches, &t->link);
}

static const lm_request_fn seat_handlers[] = {
    [LM_WL_SEAT_REQ_GET_POINTER] = seat_get_pointer,
    [LM_WL_SEAT_REQ_GET_KEYBOARD] = seat_get_keyboard,
    [LM_WL_SEAT_REQ_GET_TOUCH] = seat_get_touch,
    [LM_WL_SEAT_REQ_RELEASE] = generic_release,
};

static uint32_t seat_capabilities(const struct lm_seat *seat)
{
    uint32_t caps = LM_SEAT_CAP_POINTER | LM_SEAT_CAP_KEYBOARD;
    if (seat->has_touch)
        caps |= LM_SEAT_CAP_TOUCH;
    return caps;
}

static void seat_binding_destroy(struct lm_resource *res)
{
    struct seat_binding *b = res->data;
    lm_list_remove(&b->link);
    free(b);
}

static void seat_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_seat *seat = data;
    struct seat_binding *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->seat = seat;
    b->res = lm_resource_create(client, id, &lm_wl_seat_interface, version, seat_handlers, b,
                                seat_binding_destroy);
    if (!b->res) {
        free(b);
        return;
    }
    lm_list_append(&seat->bindings, &b->link);
    lm_resource_post(b->res, LM_WL_SEAT_EVT_CAPABILITIES, seat_capabilities(seat));
    lm_resource_post(b->res, LM_WL_SEAT_EVT_NAME, SEAT_NAME);
}

static void seat_note_touch_device(struct lm_seat *seat)
{
    if (seat->has_touch)
        return;
    seat->has_touch = true;
    struct seat_binding *b;
    lm_list_for_each(b, &seat->bindings, link)
        lm_resource_post(b->res, LM_WL_SEAT_EVT_CAPABILITIES, seat_capabilities(seat));
}

/* ---- touch ------------------------------------------------------------------------------------------------------ */

static void touch_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct touch_point *tp = lm_container_of(l, struct touch_point, destroy);
    lm_listener_remove(l);
    tp->active = false;
    tp->surface = NULL;
}

static struct touch_point *touch_find(struct lm_seat *seat, int32_t id)
{
    for (int i = 0; i < MAX_TOUCH; i++)
        if (seat->points[i].active && seat->points[i].id == id)
            return &seat->points[i];
    return NULL;
}

static void touch_release_point(struct touch_point *tp)
{
    if (tp->surface)
        lm_listener_remove(&tp->destroy);
    tp->active = false;
    tp->surface = NULL;
}

static bool touch_global_pos(struct lm_seat *seat, double nx, double ny, double *gx, double *gy)
{
    struct lm_rect b;
    if (!lm_output_layout_bounds(seat->srv, &b))
        return false;
    nx = isfinite(nx) ? LM_CLAMP(nx, 0.0, 1.0) : 0.0;
    ny = isfinite(ny) ? LM_CLAMP(ny, 0.0, 1.0) : 0.0;
    *gx = b.x + nx * b.w;
    *gy = b.y + ny * b.h;
    return true;
}

static void touch_down(struct lm_seat *seat, const struct lm_input_event *ev)
{
    int32_t id = ev->u.touch.id;
    double gx, gy;
    if (!touch_global_pos(seat, ev->u.touch.x, ev->u.touch.y, &gx, &gy))
        return;
    if (touch_find(seat, id))
        return;
    struct touch_point *tp = NULL;
    for (int i = 0; i < MAX_TOUCH; i++) {
        if (!seat->points[i].active) {
            tp = &seat->points[i];
            break;
        }
    }
    if (!tp)
        return;

    struct lm_wm *wm = seat->srv->wm;
    struct lm_pick pick;
    memset(&pick, 0, sizeof(pick));
    if (wm)
        lm_wm_pick(wm, gx, gy, &pick);

    memset(tp, 0, sizeof(*tp));
    lm_list_init(&tp->destroy.link);
    tp->seat = seat;
    tp->id = id;

    if (pick_is_surface(&pick)) {
        /* Let the wm focus/raise the touched window (touch acts like a left click). */
        if (wm)
            lm_wm_pointer_button(wm, &pick, ev->time_ms, BTN_LEFT, true);
        struct lm_surface *s = pick.surface;
        tp->active = true;
        tp->surface = s;
        tp->ox = gx - pick.sx;
        tp->oy = gy - pick.sy;
        tp->destroy.notify = touch_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &tp->destroy);

        struct lm_client *cl = surface_client(s);
        uint32_t serial = next_serial(seat);
        serial_record(seat, cl, serial);
        struct seat_touch *t;
        FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_DOWN, serial, ev->time_ms, s->resource, id,
                             lm_fixed_from_double(pick.sx), lm_fixed_from_double(pick.sy));
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
        }
    } else {
        /* Decorations, panel: emulate a pointer press at the touch position. */
        tp->active = true;
        tp->via_pointer = true;
        pointer_set_position(seat, gx, gy, ev->time_ms);
        pointer_button(seat, ev->time_ms, BTN_LEFT, true);
    }
}

static void touch_motion(struct lm_seat *seat, const struct lm_input_event *ev)
{
    struct touch_point *tp = touch_find(seat, ev->u.touch.id);
    double gx, gy;
    if (!tp || !touch_global_pos(seat, ev->u.touch.x, ev->u.touch.y, &gx, &gy))
        return;
    if (tp->via_pointer) {
        pointer_set_position(seat, gx, gy, ev->time_ms);
        return;
    }
    if (!tp->surface)
        return;
    struct lm_client *cl = surface_client(tp->surface);
    lm_fixed_t fx = lm_fixed_from_double(gx - tp->ox);
    lm_fixed_t fy = lm_fixed_from_double(gy - tp->oy);
    struct seat_touch *t;
    FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
        lm_resource_post(t->res, LM_WL_TOUCH_EVT_MOTION, ev->time_ms, tp->id, fx, fy);
        lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
    }
}

static void touch_up(struct lm_seat *seat, const struct lm_input_event *ev)
{
    struct touch_point *tp = touch_find(seat, ev->u.touch.id);
    if (!tp)
        return;
    if (tp->via_pointer) {
        tp->active = false;
        pointer_button(seat, ev->time_ms, BTN_LEFT, false);
        return;
    }
    if (tp->surface) {
        struct lm_client *cl = surface_client(tp->surface);
        uint32_t serial = next_serial(seat);
        struct seat_touch *t;
        FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_UP, serial, ev->time_ms, tp->id);
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
        }
    }
    touch_release_point(tp);
}

static void touch_cancel_all(struct lm_seat *seat, uint32_t time_ms)
{
    struct lm_client *done[MAX_TOUCH];
    int n_done = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        struct touch_point *tp = &seat->points[i];
        if (!tp->active)
            continue;
        if (tp->via_pointer) {
            tp->active = false;
            pointer_button(seat, time_ms, BTN_LEFT, false);
            continue;
        }
        struct lm_client *cl = surface_client(tp->surface);
        bool seen = false;
        for (int j = 0; j < n_done; j++)
            seen |= done[j] == cl;
        if (cl && !seen) {
            done[n_done++] = cl;
            struct seat_touch *t;
            FOR_CLIENT_OBJECTS(t, &seat->touches, cl)
                lm_resource_post(t->res, LM_WL_TOUCH_EVT_CANCEL);
        }
        touch_release_point(tp);
    }
}

/* ---- wl_data_device_manager ------------------------------------------------------------------------------------- */

static struct data_source *source_of(struct lm_resource *res)
{
    return res ? res->data : NULL;
}

static void offer_res_destroy(struct lm_resource *res)
{
    struct data_offer *o = res->data;
    lm_list_remove(&o->link);
    lm_list_remove(&o->src_link);
    free(o);
}

static void offer_accept(struct lm_resource *res, const union lm_arg *args)
{
    struct data_offer *o = res->data;
    (void)args;
    (void)o; /* drag-and-drop is not performed: nothing to negotiate */
}

static void offer_receive(struct lm_resource *res, const union lm_arg *args)
{
    struct data_offer *o = res->data;
    const char *mime = args[0].s;
    if (!o->source || !mime)
        return;
    for (int i = 0; i < o->source->n_mimes; i++) {
        if (strcmp(o->source->mimes[i], mime) == 0) {
            /* the wire layer dup()s the fd; the server closes ours after this handler */
            lm_resource_post(o->source->res, LM_WL_DATA_SOURCE_EVT_SEND, mime, args[1].h);
            return;
        }
    }
}

static void offer_finish(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_FINISH,
                           "wl_data_offer.finish on a selection offer");
}

static void offer_set_actions(struct lm_resource *res, const union lm_arg *args)
{
    uint32_t actions = args[0].u, preferred = args[1].u;
    if (actions & ~7u) {
        lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_ACTION_MASK,
                               "invalid action mask %u", actions);
        return;
    }
    if (preferred != 0 && (preferred & (preferred - 1)) != 0) {
        lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_ACTION,
                               "invalid preferred action %u", preferred);
    }
}

static const lm_request_fn offer_handlers[] = {
    [LM_WL_DATA_OFFER_REQ_ACCEPT] = offer_accept,
    [LM_WL_DATA_OFFER_REQ_RECEIVE] = offer_receive,
    [LM_WL_DATA_OFFER_REQ_DESTROY] = generic_release,
    [LM_WL_DATA_OFFER_REQ_FINISH] = offer_finish,
    [LM_WL_DATA_OFFER_REQ_SET_ACTIONS] = offer_set_actions,
};

/* Introduce the current selection to one data device. */
static void device_send_selection(struct lm_seat *seat, struct data_device *dev)
{
    struct data_source *src = seat->selection;
    if (!src) {
        lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_SELECTION, (struct lm_resource *)NULL);
        return;
    }
    struct data_offer *o = calloc(1, sizeof(*o));
    if (!o) {
        lm_resource_post_no_memory(dev->res);
        return;
    }
    o->seat = seat;
    o->source = src;
    lm_list_init(&o->link);
    lm_list_init(&o->src_link);
    o->res = lm_resource_create_server(dev->res->client, &lm_wl_data_offer_interface,
                                       dev->res->version, offer_handlers, o, offer_res_destroy);
    if (!o->res) {
        free(o);
        return;
    }
    lm_list_append(&seat->offers, &o->link);
    lm_list_append(&src->offers, &o->src_link);
    lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_DATA_OFFER, o->res);
    for (int i = 0; i < src->n_mimes; i++)
        lm_resource_post(o->res, LM_WL_DATA_OFFER_EVT_OFFER, src->mimes[i]);
    lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_SELECTION, o->res);
}

/* Tell the keyboard-focus client about the current selection. */
static void selection_update_focus(struct lm_seat *seat)
{
    if (seat->dying || !seat->kfocus)
        return;
    struct lm_client *cl = surface_client(seat->kfocus);
    struct data_device *d;
    FOR_CLIENT_OBJECTS(d, &seat->devices, cl)
        device_send_selection(seat, d);
}

static void seat_set_selection(struct lm_seat *seat, struct data_source *src)
{
    if (seat->selection == src)
        return;
    struct data_source *old = seat->selection;
    seat->selection = src;
    if (old)
        lm_resource_post(old->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
    selection_update_focus(seat);
}

static void source_res_destroy(struct lm_resource *res)
{
    struct data_source *src = res->data;
    struct lm_seat *seat = src->seat;
    struct data_offer *o, *tmp;
    lm_list_for_each_safe(o, tmp, &src->offers, src_link) {
        o->source = NULL;
        lm_list_remove(&o->src_link);
    }
    lm_list_remove(&src->link);
    if (seat->selection == src) {
        seat->selection = NULL;
        selection_update_focus(seat);
    }
    for (int i = 0; i < src->n_mimes; i++)
        free(src->mimes[i]);
    free(src);
}

static void source_offer(struct lm_resource *res, const union lm_arg *args)
{
    struct data_source *src = res->data;
    const char *mime = args[0].s;
    if (!mime || !*mime || strlen(mime) > MAX_MIME_LEN || src->n_mimes >= MAX_MIMES)
        return;
    for (int i = 0; i < src->n_mimes; i++)
        if (strcmp(src->mimes[i], mime) == 0)
            return;
    char *copy = strdup(mime);
    if (!copy) {
        lm_resource_post_no_memory(res);
        return;
    }
    src->mimes[src->n_mimes++] = copy;
}

static void source_set_actions(struct lm_resource *res, const union lm_arg *args)
{
    struct data_source *src = res->data;
    if (args[0].u & ~7u) {
        lm_resource_post_error(res, LM_WL_DATA_SOURCE_ERR_INVALID_ACTION_MASK,
                               "invalid action mask %u", args[0].u);
        return;
    }
    src->dnd_actions = args[0].u;
}

static const lm_request_fn source_handlers[] = {
    [LM_WL_DATA_SOURCE_REQ_OFFER] = source_offer,
    [LM_WL_DATA_SOURCE_REQ_DESTROY] = generic_release,
    [LM_WL_DATA_SOURCE_REQ_SET_ACTIONS] = source_set_actions,
};

static void device_res_destroy(struct lm_resource *res)
{
    struct data_device *d = res->data;
    lm_list_remove(&d->link);
    free(d);
}

static void device_start_drag(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    /* Drag-and-drop is not performed; refuse politely so the client resets its state. */
    struct data_source *src = source_of(args[0].r);
    if (src)
        lm_resource_post(src->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
}

static void device_set_selection(struct lm_resource *res, const union lm_arg *args)
{
    struct data_device *d = res->data;
    struct lm_seat *seat = d->seat;
    struct data_source *src = source_of(args[0].r);
    /*
     * Only the client that has the keyboard focus may own the clipboard
     * (serials are not checked: toolkits pass whatever input serial they saw last).
     */
    bool focused = seat->kfocus && surface_client(seat->kfocus) == res->client;
    if (!focused) {
        if (src)
            lm_resource_post(src->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
        return;
    }
    seat_set_selection(seat, src);
}

static const lm_request_fn device_handlers[] = {
    [LM_WL_DATA_DEVICE_REQ_START_DRAG] = device_start_drag,
    [LM_WL_DATA_DEVICE_REQ_SET_SELECTION] = device_set_selection,
    [LM_WL_DATA_DEVICE_REQ_RELEASE] = generic_release,
};

static void ddm_create_data_source(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_seat *seat = res->data;
    struct data_source *src = calloc(1, sizeof(*src));
    if (!src) {
        lm_resource_post_no_memory(res);
        return;
    }
    src->seat = seat;
    lm_list_init(&src->offers);
    src->res = lm_resource_create(res->client, args[0].n, &lm_wl_data_source_interface,
                                  res->version, source_handlers, src, source_res_destroy);
    if (!src->res) {
        free(src);
        return;
    }
    lm_list_append(&seat->sources, &src->link);
}

static void ddm_get_data_device(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_seat *seat = res->data;
    struct data_device *d = calloc(1, sizeof(*d));
    if (!d) {
        lm_resource_post_no_memory(res);
        return;
    }
    d->seat = seat;
    d->res = lm_resource_create(res->client, args[0].n, &lm_wl_data_device_interface,
                                res->version, device_handlers, d, device_res_destroy);
    if (!d->res) {
        free(d);
        return;
    }
    lm_list_append(&seat->devices, &d->link);
    if (seat->kfocus && surface_client(seat->kfocus) == res->client)
        device_send_selection(seat, d);
}

static const lm_request_fn ddm_handlers[] = {
    [LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE] = ddm_create_data_source,
    [LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE] = ddm_get_data_device,
};

static void ddm_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_seat *seat = data;
    lm_resource_create(client, id, &lm_wl_data_device_manager_interface, version, ddm_handlers,
                       seat, NULL);
}

/* ---- input entry point ---------------------------------------------------------------------------------------------- */

void lm_seat_handle_input(struct lm_seat *seat, const struct lm_input_event *ev)
{
    if (!seat || !ev || seat->dying)
        return;
    ensure_pointer_pos(seat);

    switch (ev->type) {
    case LM_INPUT_KEY:
        handle_key(seat, ev);
        break;
    case LM_INPUT_POINTER_MOTION:
        if (!seat->pos_valid)
            break;
        if (!isfinite(ev->u.motion.dx) || !isfinite(ev->u.motion.dy))
            break;
        pointer_set_position(seat, seat->px + ev->u.motion.dx, seat->py + ev->u.motion.dy,
                             ev->time_ms);
        break;
    case LM_INPUT_POINTER_MOTION_ABS: {
        double gx, gy;
        if (touch_global_pos(seat, ev->u.motion_abs.x, ev->u.motion_abs.y, &gx, &gy))
            pointer_set_position(seat, gx, gy, ev->time_ms);
        break;
    }
    case LM_INPUT_POINTER_BUTTON:
        pointer_button(seat, ev->time_ms, ev->u.button.code, ev->u.button.pressed);
        break;
    case LM_INPUT_POINTER_AXIS:
        if (isfinite(ev->u.axis.value))
            pointer_axis(seat, ev->time_ms, ev->u.axis.orientation, ev->u.axis.value,
                         ev->u.axis.discrete);
        break;
    case LM_INPUT_TOUCH_DOWN:
        seat_note_touch_device(seat);
        touch_down(seat, ev);
        break;
    case LM_INPUT_TOUCH_MOTION:
        seat_note_touch_device(seat);
        touch_motion(seat, ev);
        break;
    case LM_INPUT_TOUCH_UP:
        seat_note_touch_device(seat);
        touch_up(seat, ev);
        break;
    case LM_INPUT_TOUCH_CANCEL:
        seat_note_touch_device(seat);
        touch_cancel_all(seat, ev->time_ms);
        break;
    case LM_INPUT_TOUCH_FRAME:
        /* every touch event is already sent in its own frame */
        break;
    }
}

void lm_seat_handle_input_cb(const struct lm_input_event *ev, void *data)
{
    lm_seat_handle_input(data, ev);
}

/* ---- lifecycle ----------------------------------------------------------------------------------------------------------- */

struct lm_seat *lm_seat_create(struct lm_server *srv)
{
    struct lm_seat *seat = calloc(1, sizeof(*seat));
    if (!seat)
        return NULL;
    seat->srv = srv;
    seat->keymap_fd = -1;
    lm_list_init(&seat->bindings);
    lm_list_init(&seat->pointers);
    lm_list_init(&seat->keyboards);
    lm_list_init(&seat->touches);
    lm_list_init(&seat->devices);
    lm_list_init(&seat->sources);
    lm_list_init(&seat->offers);
    lm_list_init(&seat->clients);
    lm_list_init(&seat->pfocus_destroy.link);
    lm_list_init(&seat->kfocus_destroy.link);
    lm_list_init(&seat->cursor_destroy.link);
    for (int i = 0; i < MAX_TOUCH; i++)
        lm_list_init(&seat->points[i].destroy.link);

    seat->keymap_fd = keymap_make_fd(&seat->keymap_size);
    if (seat->keymap_fd < 0)
        lm_warn("could not create the keymap file: %s; clients get no keymap", strerror(errno));

    if (srv->loop)
        seat->refocus_idle = lm_loop_add_idle(srv->loop, refocus_idle_cb, seat);

    seat->seat_global = lm_global_create(srv, &lm_wl_seat_interface, LM_WL_SEAT_VERSION,
                                         seat_bind, seat);
    seat->ddm_global = lm_global_create(srv, &lm_wl_data_device_manager_interface,
                                        LM_WL_DATA_DEVICE_MANAGER_VERSION, ddm_bind, seat);
    if (!seat->seat_global || !seat->ddm_global) {
        lm_error("could not advertise wl_seat");
        lm_seat_destroy(seat);
        return NULL;
    }
    return seat;
}

void lm_seat_destroy(struct lm_seat *seat)
{
    if (!seat)
        return;
    seat->dying = true;

    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        seat->grab = NULL;
        if (g->ops && g->ops->cancel)
            g->ops->cancel(g);
    }
    if (seat->seat_global)
        lm_global_destroy(seat->seat_global);
    if (seat->ddm_global)
        lm_global_destroy(seat->ddm_global);
    if (seat->refocus_idle)
        lm_idle_destroy(seat->refocus_idle);

    /* Clients may outlive the seat: release every resource that points into it. */
    seat->selection = NULL;
    DESTROY_ALL(struct data_offer, &seat->offers);
    DESTROY_ALL(struct data_source, &seat->sources);
    DESTROY_ALL(struct data_device, &seat->devices);
    DESTROY_ALL(struct seat_touch, &seat->touches);
    DESTROY_ALL(struct seat_keyboard, &seat->keyboards);
    DESTROY_ALL(struct seat_pointer, &seat->pointers);
    DESTROY_ALL(struct seat_binding, &seat->bindings);

    for (int i = 0; i < MAX_TOUCH; i++)
        if (seat->points[i].active)
            touch_release_point(&seat->points[i]);
    lm_listener_remove(&seat->pfocus_destroy);
    lm_listener_remove(&seat->kfocus_destroy);
    cursor_drop(seat);
    struct seat_client *sc, *sc_tmp;
    lm_list_for_each_safe(sc, sc_tmp, &seat->clients, link) {
        lm_listener_remove(&sc->destroy);
        lm_list_remove(&sc->link);
        free(sc);
    }

    if (seat->keymap_fd >= 0)
        close(seat->keymap_fd);
    free(seat);
}
