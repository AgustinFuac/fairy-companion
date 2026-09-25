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

// Companion tint selection. Index into k_fairyTints in the .cpp.
enum FairyColor_e {
    FAIRY_COLOR_BLUE = 0,  // Navi (OoT Navi is blue/cyan)
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

    u32 mParticleId;
    u8  mParticleTimer;
    u32 mSparkleId;
    u8  mSparkleTimer;

    virtual ~maFairyCompanion_c();

    cPhs_Step create();
    int  CreateHeap();
    int  Delete();
    int  Execute();
    int  Draw();
    void setBaseMtx();

    /* Overwrites the material TEV registers. Must be declared here or the
       definition in the .cpp will not compile. */
    void applyFairyTint(J3DModel* i_model);

    /* Runtime color switch, for a future button / config binding. */
    void setColor(u8 colorId) { mColorId = colorId; }

    static int createHeapCallBack(fopAc_ac_c*);

    static s16 sProcName;
    static ActorHandle sActorHandle;
    static const ActorProfileDesc sProfile;
};