#ifndef EMOTION_H
#define EMOTION_H

#include <TJpg_Decoder.h>

#include "..\resource\emoji_h\binhthuong.h"
#include "..\resource\emoji_h\vuive.h"
#include "..\resource\emoji_h\buon.h"
#include "..\resource\emoji_h\hoamat.h"

#include "..\resource\emoji_h\buonngu.h"
#include "..\resource\emoji_h\nhaymat.h"
#include "..\resource\emoji_h\suynghi2.h"
#include "..\resource\emoji_h\ngacnhien2.h"
#include "..\resource\emoji_h\nheomat.h"
#include "..\resource\emoji_h\duamat.h"
#include "..\resource\emoji_h\doxet.h"
#include "..\resource\emoji_h\macdinh.h"

#define EMOTION_NEUTRAL      0       // Bình thường
#define EMOTION_HAPPY        1       // Vui vẻ
#define EMOTION_SAD          2       // Buồn
#define EMOTION_STUNNED      3       // Hóa đá

// List of emotions
VideoInfo* emotionList[] = { &binhthuong, &vuive, &buon, &hoamat };

// List of animations for free roaming
VideoInfo* animationList[] = { &buonngu, &nhaymat, &suynghi2, &ngacnhien2 , &nheomat, &duamat, &doxet, &macdinh };


#endif // EMOTION_H