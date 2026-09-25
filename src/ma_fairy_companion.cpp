#include "ma_fairy_companion.hpp"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"

// dKy_tevstr_init() / dKy_setLight_nowroom_actor() / g_env_light
#include "d/d_kankyo.h"

// J3DModelData::getMaterialNum() / getMaterialNodePointer()
// J3DMaterial::getTevColor() / getTevKColor()
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Same heap size daObjYOUSEI_c::create() uses for the same model + animation.
// We load exactly the same resources, so we reuse the value.
static constexpr u32 heap_size = 0x1100;

// Floor color passed to dKy_tevstr_init(). The fairy floats and has no ground
// collision, so there is no real floor poly to read a color from.
// NOTE: not verified against the decomp. If lighting looks odd, try 0.
static constexpr u8 k_defaultFloorCol = 0xFF;

// --- Tint bisect helpers ---------------------------------------------------
// Current working state is "write everything" (-1 / 0), which is what made the
// brute force test turn the fairy green. Narrow these down to stop
// overwriting registers we do not actually need:
//   k_tintKind: 0 = both, 1 = TevColor only, 2 = TevKColor only
//   k_tintReg : -1 = all 4 registers, 0..3 = only that one
static constexpr int k_tintKind = 0;
static constexpr int k_tintReg = -1;

// ---------------------------------------------------------------------------
// Color table
// ---------------------------------------------------------------------------

struct FairyTint_s {
    u8 bodyR, bodyG, bodyB;   // material TEV registers
    u8 prmR, prmG, prmB;    // trail / sparkle prmColor
    u8 envR, envG, envB;    // trail / sparkle envColor
    u8 auraR, auraG, auraB;   // 0x730 body aura color
    u8 auraAlpha;             // 0..255 - main intensity knob for the aura
};

// Indexed by FairyColor_e.
// Saturation tip: the two "off" channels control how washed out the color
// looks. Lower them for a punchier tint, raise them toward the dominant
// channel to fade it toward white.
static const FairyTint_s k_fairyTints[] = {
    // FAIRY_COLOR_BLUE - Navi RGB body RGB Particle prm: brighter, slightly washed, particle env: darker, more saturated
    {  0, 117, 235,   80, 180, 255,   20,  60, 200,    70, 170, 255,  140 },
    // FAIRY_COLOR_ORANGE - Tatl
    { 200, 150,  20,  255, 190,  70,  200,  90,  10,   255, 175,  60,  140 },
    // FAIRY_COLOR_PURPLE - Tael
    { 173,  10, 10,  102, 0, 80,  100,  20, 200,   175,  90, 255,  140 },
    // FAIRY_COLOR_GREEN - extra
    {  50, 255,  80,  110, 255, 140,   20, 180,  50,    90, 255, 120,  140 },
};

static constexpr u8 k_fairyTintCount =
(u8)(sizeof(k_fairyTints) / sizeof(k_fairyTints[0]));

static const FairyTint_s& getFairyTint(u8 colorId) {
    if (colorId >= k_fairyTintCount) {
        colorId = FAIRY_COLOR_BLUE;
    }
    return k_fairyTints[colorId];
}

// ---------------------------------------------------------------------------
// Global color selection
// ---------------------------------------------------------------------------

u8 maFairyCompanion_c::sSelectedColor = FAIRY_COLOR_BLUE;

void maFairyCompanion_c::setSelectedColor(u8 colorId) {
    sSelectedColor = (colorId < k_fairyTintCount) ? colorId : FAIRY_COLOR_BLUE;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

maFairyCompanion_c::~maFairyCompanion_c() {
    // "Always" is a permanently resident archive (that is why daObjYOUSEI_c
    // does not call dComIfG_resDelete in its destructor either). Nothing to
    // free here beyond what Delete() already releases.
}

cPhs_Step maFairyCompanion_c::create() {
    // The class constructor is not invoked automatically on actor creation:
    // it has to be called manually (same pattern as ma_Mine_c / maObj_Wrock_c).
    fopAcM_ct(this, maFairyCompanion_c);

    if (!fopAcM_entrySolidHeap(this, createHeapCallBack, heap_size)) {
        return cPhs_ERROR_e;
    }

    // Initialize the whole dKy_tevstr_c (0x388 bytes). The per-frame color
    // fields get overwritten by settingTevStruct() anyway, but the control
    // fields (Material_id, Material_use_fg, Type, ...) do not, and those
    // would otherwise be heap garbage.
    dKy_tevstr_init(&tevStr, fopAcM_GetRoomNo(this), k_defaultFloorCol);

    fopAcM_SetMtx(this, mpModelMorf->getModel()->getBaseTRMtx());
    fopAcM_SetMin(this, -50.0f, -50.0f, -50.0f);
    fopAcM_SetMax(this, 50.0f, 50.0f, 50.0f);

    mSound.init(&current.pos, &eyePos, 3, 1);

    mOrbitAngle = 0.0f;
    mSwayPhase = 0.0f;
    mBobPhase = 1.5f;
    mColorId = sSelectedColor;
    mParticleId = 0;
    mParticleTimer = 0;
    mSparkleId = 0;
    mSparkleTimer = 0;
    mAuraId = 0;
    mAuraTimer = 0;

    // Deliberately absent: ObjHit(), CareAction(), dComIfGp_att_CatchRequest
    // and self-destruct timers. This actor is entirely ours. It is
    // non-interactive by construction, not by suppressing states of a class
    // that does not belong to us.

    Execute();
    return cPhs_COMPLEATE_e;
}

int maFairyCompanion_c::CreateHeap() {
    // Same resource loading pattern as daObjYOUSEI_c::CreateHeap():
    // archive "Always", index 0x21 = model, index 0xF = animation.
    // No dComIfG_resLoad needed, "Always" is already resident.
    void* modelData = dComIfG_getObjectRes("Always", 0x21);
    if (modelData == NULL) {
        return 0;
    }

    mpModelMorf = JKR_NEW mDoExt_McaMorfSO(
        (J3DModelData*)modelData, NULL, NULL,
        (J3DAnmTransform*)dComIfG_getObjectRes("Always", 0xF),
        2, 0.4f, 0, -1, &mSound, 0x80000, 0x11000084);

    return mpModelMorf != NULL;
}

int maFairyCompanion_c::createHeapCallBack(fopAc_ac_c* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->CreateHeap();
}

int maFairyCompanion_c::Delete() {
    if (heap != NULL && mpModelMorf != NULL) {
        mpModelMorf->stopZelAnime();
    }
    this->~maFairyCompanion_c();
    return 1;
}

// ---------------------------------------------------------------------------
// Per-frame logic
// ---------------------------------------------------------------------------

int maFairyCompanion_c::Execute() {
    // Re-read the global selection every frame so a menu change applies
    // immediately instead of waiting for the actor to be recreated.
    mColorId = sSelectedColor;

    daPy_py_c* player = daPy_getPlayerActorClass();

    if (player != NULL) {
        f32 facingRad = player->current.angle.y * (3.14159265f / 32768.0f);
        f32 behindRad = facingRad + 3.14159265f;  // 180° detrás de Link

        const f32 followDist = 35.0f;
        f32 targetX = player->current.pos.x + followDist * std::sin(behindRad);
        f32 targetZ = player->current.pos.z + followDist * std::cos(behindRad);

        // Sway lateral, perpendicular a hacia dónde mira Link
        mSwayPhase += 0.025f;//d0.025f
        if (mSwayPhase > 226.2831853f) mSwayPhase -= 226.2831853f;
        f32 sway = 35.0f * std::sin(mSwayPhase);
        f32 perpRad = behindRad + 1.57079633f;
        targetX += sway * std::sin(perpRad);
        targetZ += sway * std::cos(perpRad);

        // Bob vertical
        mBobPhase += 0.035f;
        if (mBobPhase > 26.2831853f) mBobPhase -= 26.2831853f;
        f32 bob = 18.0f * std::sin(mBobPhase);

        // checkReinRide() = true en caballo o jabalí
        daAlink_c* alink = daAlink_getAlinkActorClass();
        const f32 heightOffset = (alink != NULL && alink->checkReinRide()) ? 80.0f : 160.0f;


        const f32 followSpeed = 0.24f;
        current.pos.x += (targetX - current.pos.x) * followSpeed;
        current.pos.y += (player->current.pos.y + heightOffset + bob - current.pos.y) * followSpeed;
        current.pos.z += (targetZ - current.pos.z) * followSpeed;

        shape_angle.y = fopAcM_searchPlayerAngleY(this);
    }

    // Keep the tevStr in sync with the current room, then refresh the room
    // light data. This replaces what ma_Mine_c / maObj_Wrock_c get out of
    // their ground collision, which we do not have (the fairy floats).
    tevStr.room_no = fopAcM_GetRoomNo(this);
    dKy_setLight_nowroom_actor(&tevStr);

    // Particle trail. Parameters 10 and 11 of dComIfGp_particle_set are
    // prmColor / envColor, so the trail is tinted to match the body.
    const FairyTint_s& tint = getFairyTint(mColorId);
    const GXColor prmColor = { tint.prmR, tint.prmG, tint.prmB, 0xFF };
    const GXColor envColor = { tint.envR, tint.envG, tint.envB, 0xFF };

    mParticleTimer++;
    if (mParticleTimer >= 8) {
        mParticleTimer = 0;
        mParticleId = dComIfGp_particle_set(
            mParticleId, 0x72F, &current.pos, &tevStr, &shape_angle,
            NULL, 0xFF, NULL, -1, &prmColor, &envColor, NULL);
    }

    mSparkleTimer++;
    if (mSparkleTimer >= 4) {
        mSparkleTimer = 0;
        mSparkleId = dComIfGp_particle_set(
            mSparkleId, 0x731, &current.pos, &tevStr, &shape_angle,
            NULL, 0xFF, NULL, -1, &prmColor, &envColor, NULL);
    }
    // Aura / glow around the fairy body (0x730).
   // NOTE: vanilla daObjYOUSEI_c only spawns this one during MoveAction,
   // not in its idle states, so "always on" is our own choice.
   //
   // Cadence is 1 here, not 8: this emitter sits ON the body, so refreshing
   // its position only every 8 frames would make it visibly lag behind.
   // Raise k_auraInterval if it turns out to be too expensive.
    static const u8 k_auraInterval = 1;

    
    mAuraTimer++;
    if (mAuraTimer >= k_auraInterval) {
        mAuraTimer = 0;

        const GXColor auraPrm = { tint.auraR, tint.auraG, tint.auraB, 0xFF };
        const GXColor auraEnv = { tint.auraR, tint.auraG, tint.auraB, 0xFF };
        // Optional: slow breathing pulse on the aura alpha.
        // mOrbitAngle already advances 0.05 rad/frame, so we reuse it as a phase.
        const f32 pulse = 0.75f + 0.25f * std::sin(mOrbitAngle * 2.0f);
        const u8  alpha = (u8)(tint.auraAlpha * pulse);

        // 7th argument is i_alpha (u8): global opacity of the emitter.
        // This is the cleanest intensity control, no need to touch the colors.
        mAuraId = dComIfGp_particle_set(
            mAuraId, 0x730, &current.pos, &tevStr, &shape_angle,
            NULL, tint.auraAlpha, NULL, -1, &auraPrm, &auraEnv, NULL);
    }

    // Advance the flight animation, same as daObjYOUSEI_c::Execute().
    mpModelMorf->play(0, dComIfGp_getReverb(fopAcM_GetRoomNo(this)));

    eyePos = attention_info.position = current.pos;
    attention_info.flags = 0;

    setBaseMtx();
    return 1;
}

void maFairyCompanion_c::setBaseMtx() {
    mDoMtx_stack_c::transS(current.pos);
    mDoMtx_stack_c::ZXYrotM(shape_angle);
    mDoMtx_stack_c::scaleM(scale);
    mpModelMorf->getModel()->setBaseTRMtx(mDoMtx_stack_c::get());
    mpModelMorf->modelCalc();
}

// ---------------------------------------------------------------------------
// Material tint
// ---------------------------------------------------------------------------
/*
void maFairyCompanion_c::applyFairyTint(J3DModel* i_model) {
    const FairyTint_s& tint = getFairyTint(mColorId);

    // NOTE: this J3DModelData is the SHARED "Always" resource. Any vanilla
    // daObjYOUSEI_c on screen uses the very same object, so it gets tinted
    // too. That is also why this must run every frame in Draw() instead of
    // once in CreateHeap(): setLightTevColorType_MAJI() rewrites these
    // registers on every single frame.
    J3DModelData* modelData = i_model->getModelData();
    u16 matNum = modelData->getMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* mat = modelData->getMaterialNodePointer(i);
        if (mat == NULL) {
            continue;
        }

        // GX exposes 4 TEV color registers and 4 konst color registers.
        for (u32 reg = 0; reg < 4; reg++) {
            if (k_tintReg >= 0 && (int)reg != k_tintReg) {
                continue;
            }

            if (k_tintKind != 2) {
                // J3DTevBlock1 / J3DTevBlockNull do not implement this and
                // return NULL, so the check is mandatory, not defensive.
                J3DGXColorS10* c = mat->getTevColor(reg);
                if (c != NULL) {
                    c->r = (s16)tint.bodyR;
                    c->g = (s16)tint.bodyG;
                    c->b = (s16)tint.bodyB;
                }
            }

            if (k_tintKind != 1) {
                J3DGXColor* k = mat->getTevKColor(reg);
                if (k != NULL) {
                    k->r = tint.bodyR;
                    k->g = tint.bodyG;
                    k->b = tint.bodyB;
                }
            }
        }
    }
}
*/

// Confirmed by bisection: the fairy body/rim colour comes from TevColor
// register 1. Writing the other registers or any TevKColor had no additional
// visible effect, so we stop touching them.
static constexpr u32 k_fairyTintReg = 1;

void maFairyCompanion_c::applyFairyTint(J3DModel* i_model) {
    const FairyTint_s& tint = getFairyTint(mColorId);

    // NOTE: shared "Always" J3DModelData. Must run every frame in Draw()
    // because setLightTevColorType_MAJI() rewrites these registers.
    J3DModelData* modelData = i_model->getModelData();
    u16 matNum = modelData->getMaterialNum();

    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* mat = modelData->getMaterialNodePointer(i);
        if (mat == NULL) continue;

        // J3DTevBlock1 / J3DTevBlockNull do not implement this and return
        // NULL, so the check is mandatory, not defensive.
        J3DGXColorS10* c = mat->getTevColor(k_fairyTintReg);
        if (c != NULL) {
            c->r = (s16)tint.bodyR;
            c->g = (s16)tint.bodyG;
            c->b = (s16)tint.bodyB;
            // Alpha untouched on purpose: it drives the blending, not the hue.
        }
    }
}


int maFairyCompanion_c::Draw() {
    J3DModel* model = mpModelMorf->getModel();

    // 1) The environment system fills tevStr from the scene lighting.
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);

    // 2) tevStr is pushed into the model's material TEV registers.
    g_env_light.setLightTevColorType_MAJI(model, &tevStr);

    // 3) Our own tint, applied AFTER step 2 because that call would
    //    otherwise overwrite everything we write.
    applyFairyTint(model);

    mpModelMorf->entryDL();
    return 1;
}

// ---------------------------------------------------------------------------
// Actor profile
// ---------------------------------------------------------------------------

static cPhs_Step maFairyCompanion_Create(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->create();
}
static int maFairyCompanion_Delete(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Delete();
}
static int maFairyCompanion_Execute(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Execute();
}
static int maFairyCompanion_Draw(void* i_this) {
    return static_cast<maFairyCompanion_c*>(i_this)->Draw();
}
static int maFairyCompanion_IsDelete(void*) {
    return 1;
}

s16         maFairyCompanion_c::sProcName = -1;
ActorHandle maFairyCompanion_c::sActorHandle = -1;

const ActorProfileDesc maFairyCompanion_c::sProfile = {
    .name = MFAIRYC_NAME,
    .priority_group = 7,  // same list ID g_profile_Obj_Yousei uses
    .process_size = sizeof(maFairyCompanion_c),
    .draw_priority = fpcDwPi_OBJ_LBOX_e,
    .status = fopAcStts_UNK_0x40000_e | fopAcStts_UNK_0x4000_e | fopAcStts_CULL_e,
    .group = fopAc_ACTOR_e,
    .cull_type = fopAc_CULLBOX_CUSTOM_e,
    .create_function = maFairyCompanion_Create,
    .delete_function = maFairyCompanion_Delete,
    .execute_function = maFairyCompanion_Execute,
    .is_delete_function = maFairyCompanion_IsDelete,
    .draw_function = maFairyCompanion_Draw,
};