#pragma once

#include "mods/svc/actor.h"

// Base actor definitions (fopAc_ac_c, tevStr, J3DModel)
#include "f_op/f_op_actor.h"

// request_of_phase_process_class / cPhs_Step
#include "SSystem/SComponent/c_phase.h"

// mDoExt_McaMorfSO and Z2Creature
#include "f_op/f_op_actor_mng.h"
#include "d/actor/d_a_player.h"

#define MFAIRYC_NAME "mFairyC"  // 7 chars, our own process name

// Index into k_fairyTints in the .cpp.
enum FairyColor_e {
    FAIRY_COLOR_BLUE = 0,  // Navi
    FAIRY_COLOR_ORANGE = 1,  // Tatl
    FAIRY_COLOR_PURPLE = 2,  // Tael
    FAIRY_COLOR_GREEN = 3,  // extra
};

class maFairyCompanion_c : public fopAc_ac_c {
public:
    mDoExt_McaMorfSO* mpModelMorf;
    Z2Creature        mSound;

    u8  mColorId;
    f32 mOrbitAngle;
    f32 mSwayPhase;
    f32 mBobPhase;

    u32 mParticleId;    // 0x72F - soft trail
    u8  mParticleTimer;
    u32 mSparkleId;     // 0x731 - star sparkle
    u8  mSparkleTimer;
    u32 mAuraId;        // 0x730 - body aura / glow   <-- NUEVO
    u8  mAuraTimer;     

    virtual ~maFairyCompanion_c();

    cPhs_Step create();
    int  CreateHeap();
    int  Delete();
    int  Execute();
    int  Draw();
    void setBaseMtx();

    /* Overwrites the material TEV registers. Runs after
       setLightTevColorType_MAJI(), which would otherwise discard it. */
    void applyFairyTint(J3DModel* i_model);

    /* Global color selection, shared by all instances and persistent across
       respawns. mod.cpp recreates the actor on scene changes, so storing the
       choice only in mColorId would reset it every time.
       Public + static so a future mod menu can change it without needing a
       pointer to the actor instance. */
    static u8   sSelectedColor;
    static void setSelectedColor(u8 colorId);
    static u8   getSelectedColor() { return sSelectedColor; }

    static int createHeapCallBack(fopAc_ac_c*);

    static s16 sProcName;
    static ActorHandle sActorHandle;
    static const ActorProfileDesc sProfile;
};