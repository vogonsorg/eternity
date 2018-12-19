/******************************* Chasecam code *****************************/
//
// Copyright (C) 2013 Simon Howard, James Haley et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
// Chasecam
//
// Follows the displayplayer. It does exactly what it says on the
// cover!
//
//--------------------------------------------------------------------------

#include <algorithm>
#include "z_zone.h"

#include "a_small.h"
#include "c_io.h"
#include "c_runcmd.h"
#include "cam_sight.h"
#include "d_main.h"
#include "d_net.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "info.h"
#include "m_collection.h"
#include "m_compare.h"
#include "p_chase.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_mobj.h"
#include "p_portalcross.h"
#include "p_tick.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_portal.h"
#include "r_state.h"

//=============================================================================
//
// Shared Camera Code
//


//=============================================================================
//
// Chasecam
//

camera_t chasecam;
int chasecam_active = 0;
static v3fixed_t pCamTarget;
#ifdef R_LINKEDPORTALS
static int pCamTargetGroupId;
#endif

                // for simplicity
#define playermobj players[displayplayer].mo
#define playerangle (playermobj->angle)

int chasecam_height;
int chasecam_dist;

//
// Z of the line at the point of intersection.
// this function is really just to cast all the
// variables to 64-bit integers
//
static int zi(int64_t dist, int64_t totaldist, int64_t ztarget, int64_t playerz)
{
   int64_t thezi;
   
   thezi = (dist * (ztarget - playerz)) / totaldist;
   thezi += playerz;
   
   return (int)thezi;
}

//
// Chase traverse context
//
struct chasetraverse_t
{
   const linkdata_t *link; // if set, a portal was passed
   fixed_t startz;         // start Z position
   v2fixed_t intersection; // intersection position
};

//
// PTR_chaseTraverse
//
// go til you hit a wall
// set the chasecam target x and ys if you hit one
// originally based on the shooting traverse function in p_maputl.c
//
static bool PTR_chaseTraverse(intercept_t *in, void *context)
{
   if(in->isaline)
   {
      line_t *li = in->d.line;

      auto &traverse = *static_cast<chasetraverse_t *>(context);
      
      // Keep using trace.attackrange even when passing portals, because dist is only used in ratio
      // with fractional part. Safer because trace.attackrange is always MELEERANGE here.
      fixed_t dist = FixedMul(trace.attackrange, in->frac);
      fixed_t frac = in->frac - FixedDiv(12*FRACUNIT, trace.attackrange);
      
      // hit line
      // position a bit closer
      
      fixed_t x = trace.dl.x + FixedMul(trace.dl.dx, frac);
      fixed_t y = trace.dl.y + FixedMul(trace.dl.dy, frac);

      // ioanch 20160225: portal lines are currently not crossed
      if(li->flags & ML_TWOSIDED)
      {  // crosses a two sided line
         if(li->pflags & PS_PASSABLE)
         {
            // Exact target pos
            v2fixed_t targpos = { trace.dl.x + trace.dl.dx, trace.dl.y + trace.dl.dy };
            // Portal stuff. Only count it if truly crossed
            // TODO: handle edge and sector portals.
            if(P_PointOnLineSide(targpos.x, targpos.y, li) && 
               !P_PointOnLineSide(trace.dl.x, trace.dl.y, li))
            {
               traverse.intersection.x = trace.dl.x + FixedMul(trace.dl.dx, in->frac);
               traverse.intersection.y = trace.dl.y + FixedMul(trace.dl.dy, in->frac);
               traverse.link = &li->portal->data.link;
               traverse.startz += FixedMul(pCamTarget.z - traverse.startz, in->frac);

               return false;
            }
         }

         // sf: find which side it hit
         
         subsector_t *ss = R_PointInSubsector (x, y);
         
         sector_t *othersector = li->backsector;
         
         if(ss->sector==li->backsector)      // other side
            othersector = li->frontsector;

         // interpolate, find z at the point of intersection
         
         int z = zi(dist, trace.attackrange, pCamTarget.z, traverse.startz);
         
         // found which side, check for intersections
         if((li->flags & ML_BLOCKING) || 
            (othersector->floorheight>z) || (othersector->ceilingheight<z)
            || (othersector->ceilingheight-othersector->floorheight
                < 40*FRACUNIT));          // hit
         else
         {
            return true;    // continue
         }
      }

      pCamTarget.x = x; // point the new chasecam target at the intersection
      pCamTarget.y = y;
      pCamTarget.z = zi(dist, trace.attackrange, pCamTarget.z, traverse.startz);
      
      // don't go any farther
      
      return false;
   }
   
   return true;
}

static void P_GetChasecamTarget()
{
   // aimfor is the preferred height of the chasecam above
   // the player
   // haleyjd: 1 unit for each degree of pitch works surprisingly well
   fixed_t aimfor = players[displayplayer].viewheight + chasecam_height * FRACUNIT
      + FixedDiv(players[displayplayer].pitch, ANGLE_1);

   trace.sin = finesine[playerangle>>ANGLETOFINESHIFT];
   trace.cos = finecosine[playerangle>>ANGLETOFINESHIFT];
   
   pCamTarget.x = playermobj->x - chasecam_dist * trace.cos;
   pCamTarget.y = playermobj->y - chasecam_dist * trace.sin;
   pCamTarget.z = playermobj->z + aimfor;

#ifdef R_LINKEDPORTALS
   pCamTargetGroupId = playermobj->groupid;
#endif

   // the intersections test mucks up the first time, but
   // aiming at something seems to cure it
   // ioanch 20160101: don't let P_AimLineAttack change global trace.attackrange
   fixed_t oldAttackRange = trace.attackrange;
   // ioanch 20160225: just change trace.attackrange, don't call P_AimLineAttack
   trace.attackrange = MELEERANGE;
   
   // check for intersections
   chasetraverse_t traverse;
   traverse.startz = playermobj->z + 28 * FRACUNIT;
   v2fixed_t travstart = { playermobj->x, playermobj->y };
   int repprotection = 0;
   do
   {
      traverse.link = nullptr;
      P_PathTraverse(travstart.x, travstart.y, pCamTarget.x, pCamTarget.y,
         PT_ADDLINES, PTR_chaseTraverse, &traverse);
      if(traverse.link)
      {
         travstart.x = traverse.intersection.x + traverse.link->deltax;
         travstart.y = traverse.intersection.y + traverse.link->deltay;
         traverse.startz += traverse.link->deltaz;
         pCamTarget.x += traverse.link->deltax;
         pCamTarget.y += traverse.link->deltay;
         pCamTarget.z += traverse.link->deltaz;
         pCamTargetGroupId = traverse.link->toid;
      }
   } while(traverse.link && repprotection++ < 64);
   trace.attackrange = oldAttackRange;

   const subsector_t *ss = R_PointInSubsector(pCamTarget.x, pCamTarget.y);
   
   fixed_t floorheight = ss->sector->floorheight;
   fixed_t ceilingheight = ss->sector->ceilingheight;

   // don't aim above the ceiling or below the floor
   pCamTarget.z = eclamp(pCamTarget.z, floorheight + 10 * FRACUNIT, ceilingheight - 10 * FRACUNIT);
}

// the 'speed' of the chasecam: the percentage closer we
// get to the target each tic
int chasecam_speed;

void P_ChaseTicker()
{
   int xdist, ydist, zdist;

   // backup current position for interpolation
   chasecam.backupPosition();

   // find the target
   P_GetChasecamTarget();
   
   // find distance to target..

   const linkoffset_t *link = P_GetLinkOffset(chasecam.groupid, pCamTargetGroupId);
   xdist = pCamTarget.x - link->x - chasecam.x;
   ydist = pCamTarget.y - link->y - chasecam.y;
   zdist = pCamTarget.z - link->z - chasecam.z;
   
   // haleyjd: patched these lines with cph's fix
   //          for overflow occuring in the multiplication
   // now move chasecam

   chasecam.groupid = R_PointInSubsector(chasecam.x, chasecam.y)->sector->groupid;
   v2fixed_t dest = P_LinePortalCrossing(chasecam.x, chasecam.y, 
      FixedMul(xdist, chasecam_speed*(FRACUNIT / 100)), 
      FixedMul(ydist, chasecam_speed*(FRACUNIT / 100)), &chasecam.groupid);
   
   chasecam.x = dest.x;
   chasecam.y = dest.y;
   chasecam.z += FixedMul(zdist, chasecam_speed*(FRACUNIT/100));
   
   chasecam.pitch = players[displayplayer].pitch;
   chasecam.angle = playerangle;
}

// console commands

VARIABLE_BOOLEAN(chasecam_active, NULL, onoff);

CONSOLE_VARIABLE(chasecam, chasecam_active, 0)
{
   if(!Console.argc)
      return;

   if(Console.argv[0]->toInt())
      P_ChaseStart();
   else
      P_ChaseEnd();
}

VARIABLE_INT(chasecam_height, NULL, -31, 100, NULL);
CONSOLE_VARIABLE(chasecam_height, chasecam_height, 0) {}

VARIABLE_INT(chasecam_dist, NULL, 10, 1024, NULL);
CONSOLE_VARIABLE(chasecam_dist, chasecam_dist, 0) {}

VARIABLE_INT(chasecam_speed, NULL, 1, 100, NULL);
CONSOLE_VARIABLE(chasecam_speed, chasecam_speed, 0) {}

void P_ChaseStart()
{
   chasecam_active = true;
   camera = &chasecam;
   P_ResetChasecam();
}

void P_ChaseEnd()
{
   chasecam_active = false;
   camera = NULL;
}

// SoM: moved globals into linetracer_t see p_maputil.h
extern linetracer_t trace;

//
// P_ResetChasecam
//
// Reset chasecam eg after teleporting etc
//
void P_ResetChasecam()
{
   if(!chasecam_active)
      return;

   if(gamestate != GS_LEVEL) 
      return; // only in level

   // find the chasecam target
   P_GetChasecamTarget();
   
   chasecam.x = pCamTarget.x;
   chasecam.y = pCamTarget.y;
   chasecam.z = pCamTarget.z;
   
#ifdef R_LINKEDPORTALS
   chasecam.groupid = pCamTargetGroupId;
#endif

   chasecam.backupPosition();
}


//==============================================================================
//
// WALK AROUND CAMERA
//
// Walk around inside demos without upsetting demo sync.
//

camera_t walkcamera;
int walkcam_active = 0;

void P_WalkTicker()
{
   ticcmd_t *walktic = &netcmds[consoleplayer][(gametic/ticdup)%BACKUPTICS];
   int look   = walktic->look;
   int fly    = walktic->fly;
   angle_t fwan, san;

   // backup position for interpolation
   walkcamera.backupPosition();

   walkcamera.angle += walktic->angleturn << 16;
   
   // looking up/down 
   // haleyjd: this is the same as new code in p_user.c, but for walkcam
   if(look)
   {
      // test for special centerview value
      if(look == -32768)
         walkcamera.pitch = 0;
      else
      {
         walkcamera.pitch -= look << 16;
         if(walkcamera.pitch < -ANGLE_1*MAXPITCHUP)
            walkcamera.pitch = -ANGLE_1*MAXPITCHUP;
         else if(walkcamera.pitch > ANGLE_1*MAXPITCHDOWN)
            walkcamera.pitch = ANGLE_1*MAXPITCHDOWN;
      }
   }

   if(fly == FLIGHT_CENTER)
      walkcamera.flying = false;
   else if(fly)
   {
      walkcamera.z += 2 * fly * FRACUNIT;
      walkcamera.flying = true;
   }

   if(walkcamera.flying && walkcamera.pitch)
   {
      angle_t an = static_cast<angle_t>(walkcamera.pitch);
      an >>= ANGLETOFINESHIFT;
      walkcamera.z -= FixedMul((ORIG_FRICTION/4)*walktic->forwardmove, finesine[an]);
   }

   // moving forward
   fwan = walkcamera.angle;
   fwan >>= ANGLETOFINESHIFT;
   walkcamera.x += FixedMul((ORIG_FRICTION / 4) * walktic->forwardmove, finecosine[fwan]);
   walkcamera.y += FixedMul((ORIG_FRICTION / 4) * walktic->forwardmove, finesine[fwan]);

   // strafing
   san = walkcamera.angle - ANG90;
   san >>= ANGLETOFINESHIFT;
   walkcamera.x += FixedMul((ORIG_FRICTION/6) * walktic->sidemove, finecosine[san]);
   walkcamera.y += FixedMul((ORIG_FRICTION/6) * walktic->sidemove, finesine[san]);

   // haleyjd: FIXME -- this could be optimized by only
   // doing a traversal when the camera actually moves, rather
   // than every frame, naively
   subsector_t *subsec = R_PointInSubsector(walkcamera.x, walkcamera.y);

   if(!walkcamera.flying)
   {
      // keep on the ground
      walkcamera.z = subsec->sector->floorheight + 41*FRACUNIT;
   }

   fixed_t maxheight = subsec->sector->ceilingheight - 8*FRACUNIT;
   fixed_t minheight = subsec->sector->floorheight   + 4*FRACUNIT;

   if(walkcamera.z > maxheight)
      walkcamera.z = maxheight;
   if(walkcamera.z < minheight)
      walkcamera.z = minheight;
}

void P_ResetWalkcam()
{
   if(gamestate != GS_LEVEL)
      return; // only in level

   sector_t *sec;
   // ioanch 20151218: fixed point mapthing coordinates
   walkcamera.x      = playerstarts[0].x;
   walkcamera.y      = playerstarts[0].y;
   walkcamera.angle  = R_WadToAngle(playerstarts[0].angle);
   walkcamera.pitch  = 0;
   walkcamera.flying = false;
   
   // haleyjd
   sec = R_PointInSubsector(walkcamera.x, walkcamera.y)->sector;
   walkcamera.z = sec->floorheight + 41*FRACUNIT;

   walkcamera.backupPosition();
}

VARIABLE_BOOLEAN(walkcam_active, NULL,    onoff);
CONSOLE_VARIABLE(walkcam, walkcam_active, cf_notnet)
{
   if(!Console.argc)
      return;

   if(Console.argv[0]->toInt())
      P_WalkStart();
   else
      P_WalkEnd();
}

void P_WalkStart()
{
   walkcam_active = true;
   camera = &walkcamera;
   P_ResetWalkcam();
}

void P_WalkEnd()
{
   walkcam_active = false;
   camera = NULL;
}

//==============================================================================
//
// Follow Cam
//
// Follows a designated target.
//

camera_t followcam;
static Mobj *followtarget;

//
// P_LocateFollowCam
//
// Find a suitable location for the followcam by finding the furthest vertex
// in its sector from which it is visible, using CAM_CheckSight, which is
// guaranteed to never disturb the state of the game engine.
//
void P_LocateFollowCam(Mobj *target, fixed_t &destX, fixed_t &destY)
{
   PODCollection<vertex_t *> cvertexes;
   sector_t *sec = target->subsector->sector;

   // Get all vertexes in the target's sector within 256 units
   for(int i = 0; i < sec->linecount; i++)
   {
      vertex_t *v1 = sec->lines[i]->v1;
      vertex_t *v2 = sec->lines[i]->v2;

      if(P_AproxDistance(v1->x - target->x, v1->y - target->y) <= 256*FRACUNIT)
         cvertexes.add(v1);
      if(P_AproxDistance(v2->x - target->x, v2->y - target->y) <= 256*FRACUNIT)
         cvertexes.add(v2);
   }

   // Sort by distance from the target, with the furthest vertex first.
   std::sort(cvertexes.begin(), cvertexes.end(), [&] (vertex_t *a, vertex_t *b)
   {
      return (P_AproxDistance(target->x - a->x, target->y - a->y) >
              P_AproxDistance(target->x - b->x, target->y - b->y));
   });

   // Find the furthest one from which the target is visible
   for(vertex_t *&v : cvertexes)
   {
      camsightparams_t camparams;

      camparams.cx       = v->x;
      camparams.cy       = v->y;
      camparams.cz       = sec->floorheight;
      camparams.cheight  = 41 * FRACUNIT;
      camparams.cgroupid = sec->groupid;
      camparams.prev     = nullptr;
      camparams.setTargetMobj(target);

      if(CAM_CheckSight(camparams))
      {
         angle_t ang = P_PointToAngle(v->x, v->y, target->x, target->y);

         // Push coordinates in slightly toward the target
         destX = v->x + 10 * finecosine[ang >> ANGLETOFINESHIFT];
         destY = v->y + 10 * finesine[ang >> ANGLETOFINESHIFT];

         return; // We've found our location
      }
   }

   // If we got here, somehow the target isn't visible... (shouldn't happen)
   // Use the target's coordinates.
   destX = target->x;
   destY = target->y;
}

//
// P_setFollowPitch
//
static void P_setFollowPitch()
{
   fixed_t aimz = followtarget->z + 41*FRACUNIT;
   fixed_t zabs = abs(aimz - followcam.z);

   fixed_t fixedang;
   double  zdist;
   bool    camlower = (followcam.z < aimz);
   double  xydist = M_FixedToDouble(P_AproxDistance(followtarget->x - followcam.x,
                                                    followtarget->y - followcam.y));

   zdist    = M_FixedToDouble(zabs);
   fixedang = (fixed_t)(atan2(zdist, xydist) * ((unsigned int)ANG180 / PI));
      
   if(fixedang > ANGLE_1 * 32)
      fixedang = ANGLE_1 * 32;

   followcam.prevpitch = followcam.pitch;
   followcam.pitch = camlower ? -fixedang : fixedang;
}

//
// P_SetFollowCam
//
// Locate the followcam at the indicated location, looking at the target.
//
void P_SetFollowCam(fixed_t x, fixed_t y, Mobj *target)
{
   subsector_t *subsec;

   followcam.x = x;
   followcam.y = y;
   P_SetTarget<Mobj>(&followtarget, target);

   followcam.angle = P_PointToAngle(followcam.x, followcam.y,
                                    followtarget->x, followtarget->y);

   subsec = R_PointInSubsector(followcam.x, followcam.y);
   followcam.z = subsec->sector->floorheight + 41*FRACUNIT;

   P_setFollowPitch();
   followcam.backupPosition();
}

void P_FollowCamOff()
{
   P_SetTarget<Mobj>(&followtarget, NULL);
}

bool P_FollowCamTicker()
{
   subsector_t *subsec;

   if(!followtarget)
      return false;

   followcam.backupPosition();

   followcam.angle = P_PointToAngle(followcam.x, followcam.y,
                                    followtarget->x, followtarget->y);

   subsec = R_PointInSubsector(followcam.x, followcam.y);
   followcam.z       = subsec->sector->floorheight + 41*FRACUNIT;
   followcam.groupid = subsec->sector->groupid;
   P_setFollowPitch();

   // still visible?
   camsightparams_t camparams;
   camparams.prev = NULL;
   camparams.setCamera(followcam, 41 * FRACUNIT);
   camparams.setTargetMobj(followtarget);

   return CAM_CheckSight(camparams);
}

#if 0
static cell AMX_NATIVE_CALL sm_chasecam(AMX *amx, cell *params)
{
   int cam_onoff = (int)params[1];

   if(gamestate != GS_LEVEL)
   {
      amx_RaiseError(amx, SC_ERR_GAMEMODE | SC_ERR_MASK);
      return -1;
   }

   if(cam_onoff)
      P_ChaseStart();
   else
      P_ChaseEnd();

   return 0;
}

static cell AMX_NATIVE_CALL sm_ischaseon(AMX *amx, cell *params)
{
   if(gamestate != GS_LEVEL)
   {
      amx_RaiseError(amx, SC_ERR_GAMEMODE | SC_ERR_MASK);
      return -1;
   }

   return chasecam_active;
}

AMX_NATIVE_INFO chase_Natives[] =
{
   { "_ToggleChasecam", sm_chasecam  },
   { "_IsChasecamOn",   sm_ischaseon },
   { NULL, NULL }
};
#endif

// EOF

