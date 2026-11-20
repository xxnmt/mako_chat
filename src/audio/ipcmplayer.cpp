#include "ipcmplayer.h"
#include "streamplayer.h"
#include "fileplayer.h"

IPcmPlayer* IPcmPlayer::create(Type type, QObject *parent)
{
    switch(type) {
    case Stream: return new StreamPlayer(parent);
    case File:   return new FilePlayer(parent);
    }
    return nullptr;
}
