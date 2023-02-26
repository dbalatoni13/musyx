#include "musyx/assert.h"
#include "musyx/musyx_priv.h"

#ifdef _DEBUG
static u8 DebugMacroSteps = 0;
#endif

static SYNTH_VOICE* macActiveMacroRoot = NULL;
static SYNTH_VOICE* macTimeQueueRoot = NULL;
static u64 macRealTime;

static void TimeQueueAdd(SYNTH_VOICE* svoice);

void macMakeActive(SYNTH_VOICE* svoice);

void macSetExternalKeyoff(SYNTH_VOICE* svoice);

static void DoSetPitch(SYNTH_VOICE* svoice);

static int SendSingleKeyOff(u32 voiceid) {
  u32 i; // r31

  if (voiceid != 0xFFFFFFFF) {

    i = voiceid & 0xFF;

    if (voiceid == synthVoice[i].id) {

      macSetExternalKeyoff(&synthVoice[i]);

      return 0;
    }
  }

  return -1;
}

static u32 ExecuteTrap(SYNTH_VOICE* svoice, u8 trapType) {
  if (svoice->trapEventAny != 0 && svoice->trapEventAddr[trapType] != NULL) {
    svoice->curAddr = svoice->trapEventCurAddr[trapType];
    svoice->addr = svoice->trapEventAddr[trapType];
    svoice->trapEventAddr[trapType] = NULL;
    macMakeActive(svoice);
    return 1;
  }

  return 0;
}

static u32 HasHWEventTrap(SYNTH_VOICE* svoice) {
  if (svoice->trapEventAny != '\0') {
    return svoice->trapEventAddr[1] != NULL;
  }
  return 0;
}

static void CheckHWEventTrap(SYNTH_VOICE* svoice) {
  if ((svoice->cFlags & 0x20) == 0 && !hwIsActive(svoice->id & 0xff)) {
    ExecuteTrap(svoice, 1);
  }
}

static u32 mcmdWait(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 w;  // r1+0x10
  u32 ms; // r29

  if ((ms = cstep->para[1] >> 0x10)) {
    if (((u8)(cstep->para[0] >> 8) & 1)) {
      if (svoice->cFlags & 8) {
        if (!(svoice->cFlags & 0x10000000000)) {
          return 0;
        }
        svoice->cFlags |= 0x40000000000;
      }
      svoice->cFlags |= 4;
    } else {
      svoice->cFlags &= ~4;
    }

    if (((u8)(cstep->para[0] >> 0x18) & 1)) {
      if (!(svoice->cFlags & 0x20) && !hwIsActive(svoice->id & 0xFF)) {
        return 0;
      }
      svoice->cFlags |= 0x40000;
    } else {
      svoice->cFlags &= ~0x40000;
    }

    if (((u8)(cstep->para[0] >> 0x10)) & 1) {
      ms = sndRand() % ms;
    }

    if (ms != 0xFFFF) {
      if ((w = ((u8)(cstep->para[1] >> 0x8) & 1) != 0)) {
        sndConvertMs(&ms);
      } else {
        sndConvertTicks(&ms, svoice);
      }

      if (w != 0) {
        if (cstep->para[1] & 1) {
          svoice->wait = svoice->macStartTime + ms;
        } else {
          svoice->wait = macRealTime + ms;
        }
      } else {
        if (cstep->para[1] & 1) {
          svoice->wait = ms;
        } else {
          svoice->wait = svoice->waitTime + ms;
        }
      }

      if (!(svoice->wait > macRealTime)) {
        svoice->waitTime = svoice->wait;
        svoice->wait = 0;
      }
    } else {
      svoice->wait = -1;
    }

    if (svoice->wait != 0) {
      if (svoice->wait != -1) {
        TimeQueueAdd(svoice);
      }
      macMakeInactive(svoice, 1);
      return 1;
    }
  }

  return 0;
}

static u32 mcmdWaitMs(SYNTH_VOICE* svoice, MSTEP* cstep) {
  *((u8*)cstep->para + 6) = 1;
  return mcmdWait(svoice, cstep);
}

static u32 mcmdEndOfMacro(SYNTH_VOICE* svoice) {
  vidRemoveVoiceReferences(svoice);
  voiceFree(svoice);
  return 1;
}

static u32 mcmdStop(SYNTH_VOICE* svoice) { return mcmdEndOfMacro(svoice); }

static u32 mcmdReturn(SYNTH_VOICE* svoice) {
  if (svoice->callStackEntryNum != 0) {
    svoice->addr = svoice->callStack[svoice->callStackIndex].addr;
    svoice->curAddr = svoice->callStack[svoice->callStackIndex].curAddr;
    svoice->callStackIndex = (svoice->callStackIndex - 1) & 3;
    --svoice->callStackEntryNum;
  }
  return 0;
}

static void mcmdIfKey(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r31
  if (svoice->curNote < ((u8)(cstep->para[0] >> 8))) {
    return;
  }

  if ((addr = (MSTEP*)dataGetMacro((cstep->para[0] >> 0x10))) != NULL) {
    svoice->addr = addr;
    svoice->curAddr = addr + (u16)cstep->para[1];
  }
}

static void mcmdIfVelocity(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr;
  if (((u8)(svoice->volume >> 0x10)) < (u8)(cstep->para[0] >> 8)) {
    return;
  }

  if (addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) {
    svoice->addr = addr;
    svoice->curAddr = addr + (u16)cstep->para[1];
  }
}

static void mcmdIfModulation(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r30
  u8 mod;      // r28

  if (svoice->midi == 0xff) {
    return;
  }
  mod = inpGetModulation(svoice) >> 7;
  if (mod < (u8)(cstep->para[0] >> 8)) {
    return;
  }

  if (addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) {
    svoice->addr = addr;
    svoice->curAddr = addr + (u16)(cstep->para[1]);
  }
}

static void mcmdIfRandom(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r31
  if ((u8)sndRand() < (u8)(cstep->para[0] >> 8)) {
    return;
  }

  if (addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) {
    svoice->addr = addr;
    svoice->curAddr = addr + (u16)cstep->para[1];
  }
}

static u32 mcmdGoto(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r31
  if ((addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) != NULL) {
    svoice->addr = addr;
    svoice->curAddr = addr + (u16)cstep->para[1];
    return 0;
  }

  return mcmdEndOfMacro(svoice);
}

static u32 mcmdGosub(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r30
  if ((addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) != NULL) {
    svoice->callStackIndex = (svoice->callStackIndex + 1) & 3;
    svoice->callStack[svoice->callStackIndex].addr = svoice->addr;
    svoice->callStack[svoice->callStackIndex].curAddr = svoice->curAddr;
    if (++svoice->callStackEntryNum > 4) {
      svoice->callStackEntryNum = 4;
    }

    svoice->addr = addr;
    svoice->curAddr = addr + (u16)cstep->para[1];
    return 0;
  }

  return mcmdEndOfMacro(svoice);
}

static void mcmdTrapEvent(SYNTH_VOICE* svoice, MSTEP* cstep) {
  MSTEP* addr; // r29
  u8 t;        // r30
  if ((addr = (MSTEP*)dataGetMacro(cstep->para[0] >> 0x10)) != NULL) {
    t = (u8)(cstep->para[0] >> 8);
    svoice->trapEventAddr[t] = addr;
    svoice->trapEventCurAddr[t] = addr + (u16)cstep->para[1];
    svoice->trapEventAny = 1;
    if (t == 0 && (svoice->cFlags & 0x10000000008) == 0x10000000008) {
      svoice->cFlags |= 0x40000000000;
    }
  }
}

static void mcmdUntrapEvent(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u8 i; // r31
  svoice->trapEventAddr[(u8)(cstep->para[0] >> 8)] = 0;

  for (i = 0; i < 3; ++i) {
    if (svoice->trapEventAddr[i] != NULL) {
      return;
    }
  }

  svoice->trapEventAny = 0;
}

static void mcmdLoop(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPlayMacro(SYNTH_VOICE* svoice, MSTEP* cstep) {
  s32 key;       // r29
  u32 new_child; // r30

  key = ((u32)svoice->orgNote + (s8)(u8)(cstep->para[0] >> 8));
  if (key < 0) {
    key = 0;
  } else if (key > 0x7f) {
    key = 0x7f;
  }

  if (svoice->fxFlag != 0) {
    key |= 0x80;
  }

  svoice->block = 1;
  new_child = macStart((u16)(cstep->para[0] >> 0x10), (u8)(cstep->para[1] >> 0x10),
                       (u8)(cstep->para[1] >> 0x18), svoice->allocId, key,
                       (u8)(svoice->volume >> 0x10), (u8)(svoice->panning[0] >> 0x10), svoice->midi,
                       svoice->midiSet, svoice->section, (u16)cstep->para[1], (u16)svoice->track, 0,
                       svoice->vGroup, svoice->studio, svoice->itdMode == 0);
  svoice->block = 0;
  if (new_child != 0xFFFFFFFF) {
    svoice->lastVID = synthVoice[(u8)new_child].vidList->vid;
    synthVoice[(u8)new_child].parent = svoice->id;
    if (svoice->child != -1) {
      synthVoice[(u8)new_child].child = svoice->child;
      synthVoice[(u8)svoice->child].parent = new_child;
    }
    svoice->child = new_child;
    if (svoice->fxFlag != 0) {
      synthFXCloneMidiSetup(&synthVoice[(u8)new_child], svoice);
    }
  } else {
    svoice->lastVID = 0xFFFFFFFF;
  }
}

static void mcmdSendKeyOff(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 voiceid; // r30
  u32 i;       // r31

  voiceid = (svoice->orgNote + (u8)(cstep->para[0] >> 8)) << 8;
  voiceid |= ((u16)(cstep->para[0] >> 16)) << 16;
  for (i = 0; i < synthInfo.voiceNum; ++i) {
    if (synthVoice[i].id == (voiceid | i)) {
      SendSingleKeyOff(voiceid | i);
    }
  }
}

static void mcmdAddAgeCounter(SYNTH_VOICE* svoice, MSTEP* cstep) {
  s16 step; // r29
  s32 age;  // r30

  step = (u16)(cstep->para[0] >> 16);
  age = (svoice->age >> 15) + step;

  if (age < 0) {
    svoice->age = 0;
  } else if (age > 0xFFFF) {
    svoice->age = 0x7fff8000;
  } else {
    svoice->age = age * 0x8000;
  }

  hwSetPriority(svoice->id & 0xFF, ((u32)svoice->prio << 24) | ((u32)svoice->age >> 15));
}

static void mcmdSetAgeCounter(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->age = (u16)(cstep->para[0] >> 0x10) << 0xf;
  hwSetPriority(svoice->id & 0xff, (u32)svoice->prio << 0x18 | svoice->age >> 0xf);
}

static void mcmdSetAgeCounterSpeed(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 time = cstep->para[1];
  if (time != 0) {
    svoice->ageSpeed = (svoice->age >> 8) / time;
  } else {
    svoice->ageSpeed = 0;
  }
}
static void mcmdSetAgeCounterByVolume(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 age; // r30

  age = (((u8)(svoice->volume >> 16) * (u16)cstep->para[1]) >> 7) + (cstep->para[0] >> 16);
  svoice->age = age > 60000 ? 0x75300000 : age * 0x8000;
  hwSetPriority(svoice->id & 0xff, (u32)svoice->prio << 0x18 | svoice->age >> 0xf);
}

static void mcmdAddPriority(SYNTH_VOICE* svoice, MSTEP* cstep) {
  s16 add;  // r30
  s16 prio; // r31
  add = (u16)(cstep->para[0] >> 16);
  prio = svoice->prio + add;
  prio = (prio < 0) ? 0 : (prio > 0xFF) ? 0xFF : prio;

  voiceSetPriority(svoice, prio);
}

static void mcmdSetPriority(SYNTH_VOICE* svoice, MSTEP* cstep) {
  voiceSetPriority(svoice, cstep->para[0] >> 8);
}

static void mcmdSendFlag(MSTEP* cstep) {
  synthGlobalVariable[(u8)(cstep->para[0] >> 8)] = (u8)(cstep->para[0] >> 16);
}

static void mcmdSetPitchWheelRange(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->pbLowerKeyRange = (u8)(cstep->para[0] >> 0x10);
  svoice->pbUpperKeyRange = (u8)(cstep->para[0] >> 8);
}

static u32 mcmdSetKey(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->curNote = (u16)(cstep->para[0] >> 8) & 0x7f;
  svoice->curDetune = (s8)(cstep->para[0] >> 0x10);
  if (voiceIsLastStarted(svoice) != 0) {
    inpSetMidiLastNote(svoice->midi, svoice->midiSet, svoice->curNote & 0xff);
  }
  cstep->para[0] = 4;
  return mcmdWait(svoice, cstep);
}

static u32 mcmdAddKey(SYNTH_VOICE* svoice, MSTEP* cstep) {
  if (cstep->para[0] >> 0x18 == 0) {
    svoice->curNote = svoice->curNote + (s16)(s8)(u8)(cstep->para[0] >> 8);
  } else {
    svoice->curNote = (u16)svoice->orgNote + (s16)(s8)(u8)(cstep->para[0] >> 8);
  }

  svoice->curNote = (s16)svoice->curNote < 0 ? 0 : svoice->curNote > 0x7f ? 0x7f : svoice->curNote;
  svoice->curDetune = (s8)(cstep->para[0] >> 0x10);

  if (voiceIsLastStarted(svoice) != 0) {
    inpSetMidiLastNote(svoice->midi, svoice->midiSet, svoice->curNote);
  }
  cstep->para[0] = 4;
  return mcmdWait(svoice, cstep);
}

static u32 mcmdLastKey(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->curNote = svoice->lastNote + (s8)(u8)(cstep->para[0] >> 8);
  svoice->curNote = (s16)svoice->curNote < 0 ? 0 : svoice->curNote > 0x7f ? 0x7f : svoice->curNote;
  svoice->curDetune = (s8)(cstep->para[0] >> 16);
  if (svoice->midi != 0xFF) {
    inpSetMidiLastNote(svoice->midi, svoice->midiSet, svoice->curNote);
  }
  cstep->para[0] = 4;

  return mcmdWait(svoice, cstep);
}

static void mcmdStartSample(SYNTH_VOICE* svoice, MSTEP* cstep) {
  static SAMPLE_INFO newsmp;
  u16 smp; // r28
  smp = cstep->para[0] >> 8;

  if (dataGetSample(smp, &newsmp) != 0) {
    return;
  }
  switch (cstep->para[0] >> 0x18) {
  case 0:
    newsmp.offset = cstep->para[1];
    break;
  case 1:
    newsmp.offset = ((u8)(0x7f - (svoice->volume >> 0x10)) * (u32)cstep->para[1]) / 0x7f;
    ;
    break;
  case 2:
    newsmp.offset = ((u8)((svoice->volume >> 0x10)) * (u32)cstep->para[1]) / 0x7f;
    break;
  default:
    newsmp.offset = 0;
    break;
  }

  if (newsmp.offset >= newsmp.length) {
    newsmp.offset = newsmp.length - 1;
  }

  hwInitSamplePlayback(svoice->id & 0xFF, smp, &newsmp, (svoice->cFlags & 0x100) == 0,
                       ((u32)svoice->prio << 24) | ((u32)svoice->age >> 15), svoice->id,
                       (svoice->cFlags & 0x80000000000) == 0, svoice->itdMode);

  svoice->sInfo = newsmp.info;

  if (svoice->playFrq != -1) {
    DoSetPitch(svoice);
  }
  svoice->cFlags |= 0x20;
  synthKeyStateUpdate(svoice);
}

static void mcmdStopSample(SYNTH_VOICE* svoice) { hwBreak(svoice->id & 0xFF); }
static void mcmdKeyOff(SYNTH_VOICE* svoice) {
  svoice->cFlags |= 0x80;
  synthKeyStateUpdate(svoice);
}

static void mcmdSetMod2Vibrato(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->vibModAddScale = (s8)(cstep->para[0] >> 8) << 8;
  if (svoice->vibModAddScale >= 0) {
    svoice->vibModAddScale += ((s16)(s8)(cstep->para[0] >> 0x10) << 8) / 100;

  } else {
    svoice->vibModAddScale -= ((s16)(s8)(cstep->para[0] >> 0x10) << 8) / 100;
  }
}

static void mcmdVibrato(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 time; // r1+0x10
  s8 kr;    // r29
  s8 cr;    // r30

  if ((u8)(cstep->para[0] >> 0x18) & 3) {
    svoice->cFlags |= 0x4000;
  } else {
    svoice->cFlags &= ~0x4000;
  }

  time = (u16)(cstep->para[1] >> 0x10);
  if ((u16)(cstep->para[1] >> 8) & 1) {
    sndConvertMs(&time);
  } else {
    sndConvertTicks(&time, svoice);
  }

  if (time) {
    svoice->cFlags |= 0x2000;
    svoice->vibPeriod = time;

    kr = (s8)(cstep->para[0] >> 8);
    cr = (s8)(cstep->para[0] >> 16);

    if (kr < 0) {
      if (cr < 0) {
        svoice->vibCentRange = -cr;
      } else {
        svoice->vibCentRange = cr;
      }

      svoice->vibKeyRange = -kr;
      svoice->vibCurTime = svoice->vibPeriod / 2;
    } else {
      if (cr < 0) {
        if (kr == 0) {
          svoice->vibCentRange = -cr;
          svoice->vibCurTime = svoice->vibPeriod / 2;
        } else {
          --kr;
          svoice->vibCentRange = 100 - cr;
          svoice->vibCurTime = 0;
        }
      } else {
        svoice->vibCentRange = cr;
        svoice->vibCurTime = 0;
      }
      svoice->vibKeyRange = kr;
    }
  } else {
    svoice->cFlags &= ~0x2000;
  }
}

static void mcmdSetupLFO(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 time;  // r1+0x14
  u32 phase; // r1+0x10
  u8 n;      // r31

  n = (u8)(cstep->para[0] >> 8);
  time = cstep->para[0] >> 0x10;
  sndConvertMs(&time);
  if (svoice->lfo[n].period != 0) {
    phase = (u16)cstep->para[1];
    sndConvertMs(&phase);
    svoice->lfo[n].time = phase;
  }
  svoice->lfo[n].period = time;
}

static void DoSetPitch(SYNTH_VOICE* svoice) {
  u32 f;    // r28
  u32 of;   // r25
  u32 i;    // r31
  u32 frq;  // r27
  u32 ofrq; // r26
  u32 no;   // r30
  s32 key;  // r24
  u8 oKey;  // r23
  static u16 kf[13] = {
      4096, 4339, 4597, 4871, 5160, 5467, 5792, 6137, 6502, 6888, 7298, 7732, 8192,
  };
}

static void mcmdSetPitch(SYNTH_VOICE* svoice, MSTEP* cstep) {
  svoice->playFrq = (u32)(cstep->para[0] >> 8);
  svoice->playFrq |= (u8)cstep->para[1];
  if (svoice->sInfo != -1) {
    DoSetPitch(svoice);
  }
}

static void mcmdSetADSR(SYNTH_VOICE* svoice, MSTEP* cstep) {
  ADSR_INFO adsr;      // r1+0x10
  ADSR_INFO* adsr_ptr; // r31
  s32 ascale;          // r29
  s32 dscale;          // r28
  float sScale;        // r63
}

static s32 midi2TimeTab[128] = {
    0,      10,     20,     30,     40,     50,     60,     70,     80,     90,     100,    110,
    110,    120,    130,    140,    150,    160,    170,    190,    200,    220,    230,    250,
    270,    290,    310,    330,    350,    380,    410,    440,    470,    500,    540,    580,
    620,    660,    710,    760,    820,    880,    940,    1000,   1000,   1100,   1200,   1300,
    1400,   1500,   1600,   1700,   1800,   2000,   2100,   2300,   2400,   2600,   2800,   3000,
    3200,   3500,   3700,   4000,   4300,   4600,   4900,   5300,   5700,   6100,   6500,   7000,
    7500,   8100,   8600,   9300,   9900,   10000,  11000,  12000,  13000,  14000,  15000,  16000,
    17000,  18000,  19000,  21000,  22000,  24000,  26000,  28000,  30000,  32000,  34000,  37000,
    39000,  42000,  45000,  49000,  50000,  55000,  60000,  65000,  70000,  75000,  80000,  85000,
    90000,  95000,  100000, 105000, 110000, 115000, 120000, 125000, 130000, 135000, 140000, 145000,
    150000, 155000, 160000, 165000, 170000, 175000, 180000, 0,
};

static void mcmdSetADSRFromCtrl(SYNTH_VOICE* svoice, MSTEP* cstep) {
  float sScale;   // r63
  ADSR_INFO adsr; // r1+0x10
}
static void mcmdSetPitchADSR(SYNTH_VOICE* svoice, MSTEP* cstep) {
  ADSR_INFO adsr;      // r1+0x10
  ADSR_INFO* adsr_ptr; // r31
  u32 sl;              // r28
  s32 ascale;          // r27
  s32 dscale;          // r26
}
u32 mcmdPitchSweep(SYNTH_VOICE* svoice, MSTEP* cstep, int num) {
  s32 delta; // r31
}

static void DoPanningSetup(SYNTH_VOICE* svoice, MSTEP* cstep, u8 pi) {
  s32 width;  // r29
  u32 mstime; // r27
  svoice->panTime[pi] = width = (cstep->para[0] >> 16);
  sndConvertMs(&svoice->panTime[pi]);
  mstime = (s8)(cstep->para[1]);
  svoice->panning[pi] = (cstep->para[0] & 0xFF00) << 8;
  svoice->panTarget[pi] = svoice->panning[pi] + mstime * 0x10000;
  if (svoice->panTime[pi] != 0) {
    svoice->panDelta[pi] = (s32)(mstime << 16) / width;
  } else {
    svoice->panDelta[pi] = (s32)(mstime << 16);
  }

  svoice->cFlags |= 0x200000000000;
}

static void mcmdSetPanning(SYNTH_VOICE* svoice, MSTEP* cstep) { DoPanningSetup(svoice, cstep, 0); }

static void mcmdSetSurroundPanning(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdSetPianoPanning(SYNTH_VOICE* svoice, MSTEP* cstep) {
  s32 delta; // r31
  s32 scale; // r30
}
static u32 TranslateVolume(u32 volume, u16 curve) {
  u8* ptr;   // r30
  u32 vlow;  // r28
  u32 vhigh; // r31
  s32 d;     // r27
}
static void mcmdScaleVolume(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u16 curve; // r29
  u16 scale; // r28
}
static void mcmdScaleVolumeDLS(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u16 scale; // r31
}
static void DoEnvelopeCalculation(SYNTH_VOICE* svoice, MSTEP* cstep, s32 start_vol) {
  u32 tvol;   // r31
  u32 time;   // r1+0x14
  s32 mstime; // r28
  u16 curve;  // r27
}
static void mcmdEnvelope(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdFadeIn(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdRandomKey(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u8 k1;     // r30
  u8 k2;     // r29
  u8 t;      // r24
  s32 i1;    // r28
  s32 i2;    // r27
  u8 detune; // r26
}
static void mcmdSetPitchbendAfterKeyOff(SYNTH_VOICE* svoice) {}
static void mcmdScaleReverb(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void SelectSource(SYNTH_VOICE* svoice, CTRL_DEST* dest, MSTEP* cstep, u64 tstflag,
                         u32 dirtyFlag) {
  u8 comb;   // r28
  s32 scale; // r30
}
static void mcmdVolumeSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPanningSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPitchWheelSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdModWheelSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPedalSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPortamentoSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdReverbSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPreAuxASelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPreAuxBSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdPostAuxBSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdSurroundPanningSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdDopplerSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdTremoloSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdAuxAFXSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 i;                                                                     // r31
  static u64 mask[4] = {0x100000000, 0x200000000, 0x400000000, 0x800000000}; // size: 0x20
  static u32 dirty[4] = {0x80000001, 0x80000002, 0x80000004, 0x80000008};    // size: 0x10
}

static void mcmdAuxBFXSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 i;                                                                         // r31
  static u64 mask[4] = {0x1000000000, 0x2000000000, 0x4000000000, 0x8000000000}; // size: 0x20
  static u32 dirty[4] = {0x80000010, 0x80000020, 0x80000040, 0x80000080};        // size: 0x10
}

static void mcmdPortamento(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 time; // r1+0x10
}
s32 varGet32(SYNTH_VOICE* svoice, u32 ctrl, u8 index) {
  if (ctrl != 0) {
    return inpGetExCtrl(svoice, index);
  }

  index &= 0x1f;
  if (index < 16) {
    return svoice->local_vars[index];
  }

  return synthGlobalVariable[index - 16];
}

s16 varGet(SYNTH_VOICE* svoice, u32 ctrl, u8 index) { return varGet32(svoice, ctrl, index); }

void varSet32(SYNTH_VOICE* svoice, u32 ctrl, u8 index, s32 v) {
  if (ctrl != 0) {
    inpSetExCtrl(svoice, index, v);
    return;
  }
  index &= 0x1f;

  if (index < 16) {
    svoice->local_vars[index] = v;
    return;
  }

  synthGlobalVariable[index - 16] = v;
}
void varSet(SYNTH_VOICE* svoice, u32 ctrl, u8 index, s16 v) { varSet32(svoice, ctrl, index, v); }
static void mcmdVarCalculation(SYNTH_VOICE* svoice, MSTEP* cstep, u8 op) {
  s16 s1; // r28
  s16 s2; // r31
  s32 t;  // r30
}
static void mcmdSetVarImmediate(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void mcmdIfVarCompare(SYNTH_VOICE* svoice, MSTEP* cstep, u8 cmp) {
  s32 a;     // r28
  s32 b;     // r27
  u8 result; // r30
}
u32 macPostMessage(u32 vid, s32 mesg) {
  SYNTH_VOICE* sv; // r31
}
static void mcmdSendMessage(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u8 i;      // r31
  s32 mesg;  // r30
  u16 macro; // r28
}
static void mcmdGetMessage(SYNTH_VOICE* svoice, MSTEP* cstep) {
  s32 mesg; // r30
}
static void mcmdGetVID(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdModeSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdSRCModeSelect(SYNTH_VOICE* svoice, MSTEP* cstep) {}
static void mcmdSetKeyGroup(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 i;    // r31
  u8 kg;    // r30
  u32 kill; // r29
}
static void mcmdSetupTremolo(SYNTH_VOICE* svoice, MSTEP* cstep) {}

static void macHandleActive(SYNTH_VOICE* svoice) {
  u8 i;                              // r29
  u8 lastNote;                       // r27
  u32 ex;                            // r30
  CHANNEL_DEFAULTS* channelDefaults; // r28
  static MSTEP cstep;

  if (svoice->cFlags & 3) {
    if (svoice->cFlags & 1) {
      svoice->cFlags &= ~1;
      hwBreak(svoice->id & 0xFF);
    }

    svoice->panning[0] = svoice->panTarget[0] = (u32)(svoice->setup.pan) << 16;
    svoice->panning[1] = svoice->panTarget[1] = 0;
    svoice->volume = (u32)(svoice->setup.vol << 16);
    svoice->volTable = 0;
    svoice->orgVolume = svoice->volume;
    svoice->midi = svoice->setup.midi;
    svoice->midiSet = svoice->setup.midiSet;
    svoice->section = svoice->setup.section;
    svoice->track = svoice->setup.track;
    svoice->itdMode = svoice->setup.itdMode;
    svoice->keyGroup = 0;
    svoice->vibModAddScale = 0;
    svoice->treScale = 0;
    inpInit(svoice);
    if ((lastNote = inpGetMidiLastNote(svoice->midi, svoice->midiSet)) != 0xFF) {
      svoice->lastNote = lastNote;
    } else {
      svoice->lastNote = svoice->orgNote;
    }

    inpSetMidiLastNote(svoice->midi, svoice->midiSet, svoice->orgNote);
    voiceSetLastStarted(svoice);
    svoice->vGroup = svoice->setup.vGroup;
    svoice->studio = svoice->setup.studio;
    svoice->portTime = 0;
    svoice->portDuration = 25600;
    svoice->portType = 0;
    if (svoice->midi != 0xff) {
      svoice->portLastCtrlState = inpGetMidiCtrl(65, svoice->midi, svoice->midiSet);
    } else {
      svoice->portLastCtrlState = 0;
    }
    channelDefaults = inpGetChannelDefaults(svoice->midi, svoice->midiSet);
    svoice->pbLowerKeyRange = channelDefaults->pbRange;
    svoice->pbUpperKeyRange = channelDefaults->pbRange;
    svoice->revVolScale = 128;
    svoice->revVolOffset = 0;
    svoice->loop = 0;
    svoice->sweepNum[0] = 0;
    svoice->sweepNum[1] = 0;
    svoice->sweepOff[0] = 0;
    svoice->sweepOff[1] = 0;
    svoice->lfo[0].period = 0;
    svoice->lfo[0].value = 0;
    svoice->lfo[0].lastValue = 0x7fff;
    svoice->lfo[1].period = 0;
    svoice->lfo[1].value = 0;
    svoice->lfo[1].lastValue = 0x7fff;

    for (i = 0; i < 3; ++i) {
      svoice->trapEventAddr[i] = NULL;
    }

    svoice->trapEventAny = 0;
    svoice->sInfo = -1;
    svoice->playFrq = -1;
    svoice->pbLast = 0x2000;
    svoice->curOutputVolume = 0;
    svoice->cFlags &= 8;
    svoice->cFlags |= 0x300000000000;
    memset(svoice->local_vars, 0, sizeof(svoice->local_vars));
    svoice->waitTime = macRealTime;
    svoice->macStartTime = macRealTime;
    synthStartSynthJobHandling(svoice);
  }

#ifdef _DEBUG
  DebugMacroSteps = 0;
#endif

  do {
#ifdef _DEBUG
    if (++DebugMacroSteps > 32) {
      break;
    }
#endif

    cstep.para[0] = svoice->curAddr->para[0];
    cstep.para[1] = svoice->curAddr->para[1];
    ++svoice->curAddr;
    ex = 0;
    switch (cstep.para[0] & 0x7f) {
    case 0x0:
      ex = mcmdEndOfMacro(svoice);
      break;
    case 0x1:
      ex = mcmdStop(svoice);
      break;
    case 0x2:
      mcmdIfKey(svoice, &cstep);
      break;
    case 0x3:
      mcmdIfVelocity(svoice, &cstep);
      break;
    case 0x4:
      ex = mcmdWait(svoice, &cstep);
      break;
    case 0x5:
      mcmdLoop(svoice, &cstep);
      break;
    case 0x6:
      ex = mcmdGoto(svoice, &cstep);
      break;
    case 0x7:
      ex = mcmdWaitMs(svoice, &cstep);
      break;
    case 0x8:
      mcmdPlayMacro(svoice, &cstep);
      break;
    case 0x9:
      mcmdSendKeyOff(svoice, &cstep);
      break;
    case 0xa:
      mcmdIfModulation(svoice, &cstep);
      break;
    case 0xb:
      mcmdSetPianoPanning(svoice, &cstep);
      break;
    case 0xc:
      mcmdSetADSR(svoice, &cstep);
      break;
    case 0xd:
      mcmdScaleVolume(svoice, &cstep);
      break;
    case 0xe:
      mcmdSetPanning(svoice, &cstep);
      break;
    case 0xf:
      mcmdEnvelope(svoice, &cstep);
      break;
    case 0x10:
      mcmdStartSample(svoice, &cstep);
      break;
    case 0x11:
      mcmdStopSample(svoice);
      break;
    case 0x12:
      mcmdKeyOff(svoice);
      break;
    case 0x13:
      mcmdIfRandom(svoice, &cstep);
      break;
    case 0x14:
      mcmdFadeIn(svoice, &cstep);
      break;
    case 0x15:
      mcmdSetSurroundPanning(svoice, &cstep);
      break;
    case 0x16:
      mcmdSetADSRFromCtrl(svoice, &cstep);
      break;
    case 0x17:
      mcmdRandomKey(svoice, &cstep);
      break;
    case 0x18:
      ex = mcmdAddKey(svoice, &cstep);
      break;
    case 0x19:
      ex = mcmdSetKey(svoice, &cstep);
      break;
    case 0x1a:
      ex = mcmdLastKey(svoice, &cstep);
      break;
    case 0x1b:
      mcmdPortamento(svoice, &cstep);
      break;
    case 0x1c:
      mcmdVibrato(svoice, &cstep);
      break;
    case 0x1d:
      ex = mcmdPitchSweep(svoice, &cstep, 0);
      break;
    case 0x1e:
      ex = mcmdPitchSweep(svoice, &cstep, 1);
      break;
    case 0x1f:
      mcmdSetPitch(svoice, &cstep);
      break;
    case 0x20:
      mcmdSetPitchADSR(svoice, &cstep);
      break;
    case 0x21:
      mcmdScaleVolumeDLS(svoice, &cstep);
      break;
    case 0x22:
      mcmdSetMod2Vibrato(svoice, &cstep);
      break;
    case 0x23:
      mcmdSetupTremolo(svoice, &cstep);
      break;
    case 0x24:
      mcmdReturn(svoice);
      break;
    case 0x25:
      ex = mcmdGosub(svoice, &cstep);
      break;
    case 0x28:
      mcmdTrapEvent(svoice, &cstep);
      break;
    case 0x29:
      mcmdUntrapEvent(svoice, &cstep);
      break;
    case 0x2a:
      mcmdSendMessage(svoice, &cstep);
      break;
    case 0x2b:
      mcmdGetMessage(svoice, &cstep);
      break;
    case 0x2c:
      mcmdGetVID(svoice, &cstep);
      break;
    case 0x30:
      mcmdAddAgeCounter(svoice, &cstep);
      break;
    case 0x31:
      mcmdSetAgeCounter(svoice, &cstep);
      break;
    case 0x32:
      mcmdSendFlag(&cstep);
      break;
    case 0x33:
      mcmdSetPitchWheelRange(svoice, &cstep);
      break;
    case 0x34:
      mcmdScaleReverb(svoice, &cstep);
      break;
    case 0x35:
      mcmdSetPitchbendAfterKeyOff(svoice);
      break;
    case 0x36:
      mcmdSetPriority(svoice, &cstep);
      break;
    case 0x37:
      mcmdAddPriority(svoice, &cstep);
      break;
    case 0x38:
      mcmdSetAgeCounterSpeed(svoice, &cstep);
      break;
    case 0x39:
      mcmdSetAgeCounterByVolume(svoice, &cstep);
      break;
    case 0x40:
      mcmdVolumeSelect(svoice, &cstep);
      break;
    case 0x41:
      mcmdPanningSelect(svoice, &cstep);
      break;
    case 0x42:
      mcmdPitchWheelSelect(svoice, &cstep);
      break;
    case 0x43:
      mcmdModWheelSelect(svoice, &cstep);
      break;
    case 0x44:
      mcmdPedalSelect(svoice, &cstep);
      break;
    case 0x45:
      mcmdPortamentoSelect(svoice, &cstep);
      break;
    case 0x46:
      mcmdReverbSelect(svoice, &cstep);
      break;
    case 0x47:
      mcmdSurroundPanningSelect(svoice, &cstep);
      break;
    case 0x48:
      mcmdDopplerSelect(svoice, &cstep);
      break;
    case 0x49:
      mcmdTremoloSelect(svoice, &cstep);
      break;
    case 0x4a:
      mcmdPreAuxASelect(svoice, &cstep);
      break;
    case 0x4b:
      mcmdPreAuxBSelect(svoice, &cstep);
      break;
    case 0x4c:
      mcmdPostAuxBSelect(svoice, &cstep);
      break;
    case 0x4d:
      mcmdAuxAFXSelect(svoice, &cstep);
      break;
    case 0x4e:
      mcmdAuxBFXSelect(svoice, &cstep);
      break;
    case 0x50:
      mcmdSetupLFO(svoice, &cstep);
      break;
    case 0x58:
      mcmdModeSelect(svoice, &cstep);
      break;
    case 0x59:
      mcmdSetKeyGroup(svoice, &cstep);
      break;
    case 0x5a:
      mcmdSRCModeSelect(svoice, &cstep);
      break;
    case 0x60:
      mcmdVarCalculation(svoice, &cstep, 0);
      break;
    case 0x61:
      mcmdVarCalculation(svoice, &cstep, 1);
      break;
    case 0x62:
      mcmdVarCalculation(svoice, &cstep, 2);
      break;
    case 0x63:
      mcmdVarCalculation(svoice, &cstep, 3);
      break;
    case 0x64:
      mcmdVarCalculation(svoice, &cstep, 4);
      break;
    case 0x65:
      mcmdSetVarImmediate(svoice, &cstep);
      break;
    case 0x70:
      mcmdIfVarCompare(svoice, &cstep, 0);
      break;
    case 0x71:
      mcmdIfVarCompare(svoice, &cstep, 1);
    }
  } while (!ex);
}

void macHandle(u32 deltaTime) {
  SYNTH_VOICE* sv;     // r31
  SYNTH_VOICE* nextSv; // r30
  u64 w;               // r28

  for (nextSv = macTimeQueueRoot; nextSv != NULL && nextSv->wait <= macRealTime;) {
    sv = nextSv->nextTimeQueueMacro;
    w = nextSv->wait;
    macMakeActive(nextSv);
    nextSv->waitTime = w;
    nextSv = sv;
  }

  nextSv = macActiveMacroRoot;
  while (nextSv != NULL) {
    if (HasHWEventTrap(nextSv) != 0) {
      CheckHWEventTrap(nextSv);
    }

    macHandleActive(nextSv);
    nextSv = nextSv->nextMacActive;
  }
  macRealTime += deltaTime;
}

void macSampleEndNotify(SYNTH_VOICE* sv) {
  if (sv->macState != MAC_STATE_YIELDED) {
    return;
  }
#line 3156 /* clang-format off */
  ASSERT(sv->addr!=NULL);
           /* clang-format on */

  if (!ExecuteTrap(sv, 1) && (sv->cFlags & 0x40000)) {
    macMakeActive(sv);
  }
}
void macSetExternalKeyoff(SYNTH_VOICE* sv) {
  sv->cFlags |= 8;
  if (!sv->addr) {
    return;
  }

  if (!(sv->cFlags & 0x10000000000)) {
    if (!ExecuteTrap(sv, 0) && (sv->cFlags & 0x4)) {
      macMakeActive(sv);
    }
  } else {
    sv->cFlags |= 0x40000000000;
  }
}

void macSetPedalState(SYNTH_VOICE* svoice, u32 state) {
  if (state != 0) {
    svoice->cFlags |= 0x10000000000;
  } else {
    if (svoice->addr && (svoice->cFlags & 0x40000000000)) {
      if (!ExecuteTrap(svoice, 0) && (svoice->cFlags & 0x4)) {
        macMakeActive(svoice);
      }
    }

    svoice->cFlags &= ~(0x10000000000 | 0x40000000000);
  }
}

static void TimeQueueAdd(SYNTH_VOICE* svoice) {
  SYNTH_VOICE* sv;     // r31
  SYNTH_VOICE* lastSv; // r30

  lastSv = NULL;
  for (sv = macTimeQueueRoot; sv != NULL && sv->wait < svoice->wait;) {
    lastSv = sv;
    sv = sv->nextTimeQueueMacro;
  }

  if (sv == NULL) {
    if (lastSv == NULL) {
      macTimeQueueRoot = svoice;
      svoice->nextTimeQueueMacro = NULL;
      svoice->prevTimeQueueMacro = NULL;
    } else {
      lastSv->nextTimeQueueMacro = svoice;
      svoice->prevTimeQueueMacro = lastSv;
      svoice->nextTimeQueueMacro = NULL;
    }
  } else {
    svoice->nextTimeQueueMacro = sv;
    if (svoice->prevTimeQueueMacro = sv->prevTimeQueueMacro) {
      sv->prevTimeQueueMacro->nextTimeQueueMacro = svoice;
    } else {
      macTimeQueueRoot = svoice;
    }
    sv->prevTimeQueueMacro = svoice;
  }
}
static void UnYieldMacro(SYNTH_VOICE* svoice, u32 disableUpdate) {
  if (svoice->wait != 0) {
    if (svoice->wait != -1) {
      if (svoice->prevTimeQueueMacro == NULL) {
        macTimeQueueRoot = svoice->nextTimeQueueMacro;
      } else {
        svoice->prevTimeQueueMacro->nextTimeQueueMacro = svoice->nextTimeQueueMacro;
      }

      if (svoice->nextTimeQueueMacro) {
        svoice->nextTimeQueueMacro->prevTimeQueueMacro = svoice->prevTimeQueueMacro;
      }
    }

    if (!disableUpdate) {
      synthForceLowPrecisionUpdate(svoice);
    }

    svoice->wait = 0;
    svoice->waitTime = macRealTime;
    svoice->cFlags &= ~0x40004;
  }
}
void macMakeActive(SYNTH_VOICE* sv) {
  if (sv->macState == MAC_STATE_RUNNABLE) {
    return;
  }
#line 3297 /* clang-format off */
  ASSERT(sv->addr!=NULL);
           /* clang-format on */
  UnYieldMacro(sv, 0);
  if (sv->nextMacActive = macActiveMacroRoot) {
    macActiveMacroRoot->prevMacActive = sv;
  }
  sv->prevMacActive = NULL;
  macActiveMacroRoot = sv;
  sv->macState = MAC_STATE_RUNNABLE;
}

void macMakeInactive(SYNTH_VOICE* svoice, MAC_STATE newState) {
  if (svoice->macState == newState) {
    return;
  }

#line 3333 /* clang-format off */
  ASSERT(svoice->addr!=NULL);
           /* clang-format on */
  if (svoice->macState == MAC_STATE_RUNNABLE) {
    if (svoice->prevMacActive == NULL) {
      macActiveMacroRoot = svoice->nextMacActive;
    } else {
      svoice->prevMacActive->nextMacActive = svoice->nextMacActive;
    }

    if (svoice->nextMacActive != NULL) {
      svoice->nextMacActive->prevMacActive = svoice->prevMacActive;
    }
  }

  if (newState == MAC_STATE_STOPPED) {
    UnYieldMacro(svoice, 1);
  }
  svoice->macState = newState;
}

u32 macStart(u16 macid, u8 priority, u8 maxVoices, u16 allocId, u8 key, u8 vol, u8 panning, u8 midi,
             u8 midiSet, u8 section, u16 step, u16 trackid, u8 new_vid, u8 vGroup, u8 studio,
             u32 itd) {
  u32 voice;           // r30
  u32 vid;             // r25
  MSTEP* addr;         // r28
  SYNTH_VOICE* svoice; // r31
  u16 seqPrio;         // r24
}

void macInit() {
  u32 i; // r31

  macActiveMacroRoot = 0;
  macTimeQueueRoot = 0;
  macRealTime = 0;
  for (i = 0; i < synthInfo.voiceNum; ++i) {
    synthVoice[i].addr = NULL;
    synthVoice[i].macState = MAC_STATE_STOPPED;
    synthVoice[i].loop = 0;
  }
}
