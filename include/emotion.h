#ifndef EMOTION_H
#define EMOTION_H

#include <TJpg_Decoder.h>

#include "..\resource\emoji_h\binhthuong.h"
#include "..\resource\emoji_h\vuive.h"
#include "..\resource\emoji_h\buon.h"
#include "..\resource\emoji_h\hoamat.h"

#define EMOTION_NEUTRAL      0       // Bình thường
#define EMOTION_HAPPY        1       // Vui vẻ
#define EMOTION_SAD          2       // Buồn
#define EMOTION_STUNNED      3       // Hóa đá

VideoInfo* videoList[] = { &binhthuong, &vuive, &buon, &hoamat };



#endif // EMOTION_H