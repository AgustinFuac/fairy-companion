#pragma once
#include "mods/svc/actor.h"

// Definiciones base de actor
#include "f_op/f_op_actor.h"

// request_of_phase_process_class / cPhs_Step
#include "SSystem/SComponent/c_phase.h"

// mDoExt_McaMorfSO y Z2Creature (mismos includes que usa d_a_obj_yousei.h
// para tener acceso a estos mismos tipos)
#include "f_op/f_op_actor_mng.h"
#include "d/actor/d_a_player.h"

#define MFAIRYC_NAME "mFairyC"  // 7 caracteres, nombre de proceso propio del mod

// Placeholder para una futura seleccion de color (Navi/Tatl/Tael). Por ahora no
// tiene efecto todavia: seguimos investigando el mecanismo correcto de tinte
// (ver conversacion - dKy_tevstr_c / material del modelo).
enum FairyColor_e {
    FAIRY_COLOR_GREEN = 0,   // Navi
    FAIRY_COLOR_ORANGE = 1,  // Tatl
    FAIRY_COLOR_PURPLE = 2,  // Tael
};

class maFairyCompanion_c : public fopAc_ac_c {
public:
    mDoExt_McaMorfSO* mpModelMorf;
    Z2Creature mSound;
    u8 mColorId;
    f32 mOrbitAngle;
    u32 mParticleId;
    u8 mParticleTimer;
    u32 mSparkleId;
    u8 mSparkleTimer;
    u32 mParticleIdA;   // handle del emisor de particulas principal (0x72F)
    u32 mParticleIdB;   // handle del emisor secundario, solo se usa si mParticleLevel >= 2
    u8 mParticleLevel;  // 0 = sin particulas, 1 = pocas, 2 = normal

    virtual ~maFairyCompanion_c();
    cPhs_Step create();
    int CreateHeap();
    int Delete();
    int Execute();
    int Draw();
    void setBaseMtx();
    static int createHeapCallBack(fopAc_ac_c*);

    static s16 sProcName;
    static ActorHandle sActorHandle;
    static const ActorProfileDesc sProfile;
};
