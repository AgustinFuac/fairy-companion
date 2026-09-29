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

// Only Left/Right are viable as the fairy-ability trigger: A/B/R all
// overlap native actions while an enemy is locked on, and A/B share the
// same underlying status field in this game. See the note in Execute().
enum FairyButton_e {
    FAIRY_BUTTON_LEFT = 0,
    FAIRY_BUTTON_RIGHT = 1,
};

class maFairyCompanion_c : public fopAc_ac_c {
public:
    mDoExt_McaMorfSO* mpModelMorf;
    Z2Creature        mSound;

    u8  mColorId;
    f32 mOrbitAngle;  // reused as a base phase; kept for the aura's breathing pulse

    // Movimiento: sigue detrás de Link con sway lateral y bob vertical,
    // en vez de orbitar 360°. Ver Execute().
    f32 mSwayPhase;
    f32 mBobPhase;

    u32 mParticleId;    // 0x72F - soft trail
    u8  mParticleTimer;
    u32 mSparkleId;     // 0x731 - star sparkle
    u8  mSparkleTimer;
    u32 mAuraId;        // 0x730 - body aura / glow
    u8  mAuraTimer;

    //Navi talt frozen effect
    u32 mFrozenId;       // ID del enemigo congelado (k_noActorId = ninguno)
    s16 mFrozenTimer;    // frames restantes
    void releaseFrozen();
    bool mAffectSlow;    // true = Navi (ralentizar), false = Tatl (congelar)

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

    /* Whether the fairy's dKy_WolfEyeLight_set light is on. Same pattern as
       sSelectedColor: static so the UI toggle in mod.cpp can flip it without
       needing a pointer to the actor instance, and re-read every frame in
       Execute() so it applies immediately. */
    static bool sLightEnabled;
    static void setLightEnabled(bool enabled) { sLightEnabled = enabled; }
    static bool getLightEnabled() { return sLightEnabled; }

    /* Which button triggers the fairy ability combo against a locked-on
       enemy. Only Left/Right are offered (see FairyButton_e). Values above
       FAIRY_BUTTON_RIGHT (e.g. leftover 2/3 = old A/B from a config.json
       saved before this change) fall back to Left. */
    static u8   sActionButton;
    static void setActionButton(u8 button) {
        sActionButton = (button <= FAIRY_BUTTON_RIGHT) ? button : FAIRY_BUTTON_LEFT;
    }

    static int createHeapCallBack(fopAc_ac_c*);

    static s16 sProcName;
    static ActorHandle sActorHandle;
    static const ActorProfileDesc sProfile;
};