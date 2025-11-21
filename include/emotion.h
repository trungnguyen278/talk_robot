#ifndef EMOTION_H
#define EMOTION_H

#include <TJpg_Decoder.h>

#include "..\resource\ptit.h"

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
#include "..\resource\emoji_h\camlang.h"
#include "..\resource\emoji_h\chamhoi.h"
#include "..\resource\emoji_h\cuoito.h"
#include "..\resource\emoji_h\khongchiudau.h"
#include "..\resource\emoji_h\tucgian.h"
#include "..\resource\emoji_h\buon2.h"
#include "..\resource\emoji_h\domohoi.h"
#include "..\resource\emoji_h\tucgian2.h"
#include "..\resource\emoji_h\vuimung.h"

#define EMOTION_NEUTRAL      0       // Bình thường
#define EMOTION_HAPPY        1       // Vui vẻ
#define EMOTION_SAD          2       // Buồn


// logo
VideoInfo* logoPTIT = &ptit;

// List of emotions
VideoInfo* emotionList[] = { &binhthuong, &vuive, &buon };

// Stunned emotion
VideoInfo* stunnedEmotion = &hoamat;

// Thinking emotion
VideoInfo* thinkingEmotion = &suynghi2;

// List of animations for free roaming
VideoInfo* animationList[] = { &buonngu, &nhaymat, &ngacnhien2 , &nheomat, &duamat, &doxet, &macdinh, 
                              &camlang, &chamhoi, &cuoito, &khongchiudau, &tucgian, &buon2, &domohoi, &tucgian2, &vuimung };



#endif // EMOTION_H