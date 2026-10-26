#pragma once

// nightreign.exe globals, as offsets from the image base 0x140000000.
#define SESSION_MAN_RVA 0x3c21ec8     // session state at +0xc: 3 online host, 6 guest
#define WORLD_CHR_MAN_RVA 0x3c0f0a8   // PlayerIns at +0x174e8, every ChrIns in the vector at +0x17b98
#define GAME_DATA_MAN_RVA 0x3c078d0   // PlayerGameData at +8
#define MAP_ITEM_MAN_RVA 0x3c10b68
#define WORLD_AREA_TIME_RVA 0x3c13158 // WorldPlayAreaManagerImp, day clock at +0x100
#define GAME_MAN_RVA 0x3c13258
#define NET_MAN_RVA 0x3c046a8         // leave reason at +0x13, join kind 4 and 5 objects at +0x48 and +0x50
