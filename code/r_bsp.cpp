// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// DESCRIPTION:
//		BSP traversal, handling of LineSegs for rendering.
//
// This file contains some code from the Build Engine.
//
// "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
// Ken Silverman's official web site: "http://www.advsys.net/ken"
// See the included license file "BUILDLIC.TXT" for license info.
//
//-----------------------------------------------------------------------------


#include <stdlib.h>
#include <search.h>

#include "m_alloc.h"

#include "doomdef.h"

#include "m_bbox.h"

#include "i_system.h"

#include "r_main.h"
#include "r_plane.h"
#include "r_draw.h"
#include "r_things.h"

// State.
#include "doomstat.h"
#include "r_state.h"
#include "r_bsp.h"
#include "v_palette.h"

int WallMost (short *mostbuf, const secplane_t &plane);

seg_t*			curline;
side_t* 		sidedef;
line_t* 		linedef;
sector_t*		frontsector;
sector_t*		backsector;

// killough 4/7/98: indicates doors closed wrt automap bugfix:
int				doorclosed;

extern bool		rw_prepped;
extern bool		rw_havehigh, rw_havelow;
extern int		rw_floorstat, rw_ceilstat;
extern bool		rw_mustmarkfloor, rw_mustmarkceiling;
extern short	walltop[MAXWIDTH];	// [RH] record max extents of wall
extern short	wallbottom[MAXWIDTH];
extern short	wallupper[MAXWIDTH];
extern short	walllower[MAXWIDTH];

fixed_t			rw_backcz1, rw_backcz2;
fixed_t			rw_backfz1, rw_backfz2;
fixed_t			rw_frontcz1, rw_frontcz2;
fixed_t			rw_frontfz1, rw_frontfz2;


int				MaxDrawSegs;
drawseg_t		*drawsegs;
drawseg_t*		firstdrawseg;
drawseg_t*		ds_p;

fixed_t			WallTX1, WallTX2;	// x coords at left, right of wall in view space
fixed_t			WallTY1, WallTY2;	// y coords at left, right of wall in view space
fixed_t			WallTZ1, WallTZ2;	// z coords at left, right of wall in view space

fixed_t			WallCX1, WallCX2;	// x coords at left, right of wall in view space
fixed_t			WallCY1, WallCY2;	// y coords at left, right of wall in view space

fixed_t			WallSX1, WallSX2;	// x coords at left, right of wall in screen space
fixed_t			WallSZ1, WallSZ2;	// depth at left, right of wall in screen space

float			WallDepthOrg, WallDepthScale;
float			WallUoverZorg, WallUoverZstep;
float			WallInvZorg, WallInvZstep;

int WindowLeft, WindowRight;
WORD MirrorFlags;
seg_t *ActiveWallMirror;
TArray<ptrdiff_t> WallMirrors;

CVAR (Bool, r_drawflat, false, 0)		// [RH] Don't texture segs?


void R_StoreWallRange (int start, int stop);

//
// R_ClearDrawSegs
//
void R_ClearDrawSegs (void)
{
	if (drawsegs == NULL)
	{
		MaxDrawSegs = 256;		// [RH] Default. Increased as needed.
		firstdrawseg = drawsegs = (drawseg_t *)Malloc (MaxDrawSegs * sizeof(drawseg_t));
	}
	ds_p = drawsegs;
}



//
// ClipWallSegment
// Clips the given range of columns
// and includes it in the new clip list.
//
//
// 1/11/98 killough: Since a type "short" is sufficient, we
// should use it, since smaller arrays fit better in cache.
//

typedef struct {
	short first, last;		// killough
} cliprange_t;


// newend is one past the last valid seg
static cliprange_t     *newend;
static cliprange_t		solidsegs[MAXWIDTH/2+2];
static cliprange_t		*lastsolidseg;



//==========================================================================
//
// R_ClipWallSegment
//
// Clips the given range of columns, possibly including it in the clip list.
// Handles both windows (e.g. LineDefs with upper and lower textures) and
// solid walls (e.g. single sided LineDefs [middle texture]) that entirely
// block the view.
//
//==========================================================================

void R_ClipWallSegment (int first, int last, bool solid)
{
	cliprange_t *next, *start;
	int i, j;

	if (curline->linedef - lines == 2)
		last=last;
	// Find the first range that touches the range
	// (adjacent pixels are touching).
	start = solidsegs;
	while (start->last < first)
		start++;

	if (first < start->first)
	{
		if (last <= start->first)
		{
			// Post is entirely visible (above start).
			R_StoreWallRange (first, last);

			// Insert a new clippost for solid walls.
			if (solid)
			{
				if (last == start->first)
				{
					start->first = first;
				}
				else
				{
					next = newend;
					newend++;
					while (next != start)
					{
						*next = *(next-1);
						next--;
					}
					next->first = first;
					next->last = last;
				}
			}
			return;
		}

		// There is a fragment above *start.
		R_StoreWallRange (first, start->first);

		// Adjust the clip size for solid walls
		if (solid)
		{
			start->first = first;
		}
	}

	// Bottom contained in start?
	if (last <= start->last)
		return;

	next = start;
	while (last >= (next+1)->first)
	{
		// There is a fragment between two posts.
		R_StoreWallRange (next->last, (next+1)->first);
		next++;
		
		if (last <= next->last)
		{
			// Bottom is contained in next.
			last = next->last;
			goto crunch;
		}
	}

	// There is a fragment after *next.
	R_StoreWallRange (next->last, last);

crunch:
	if (solid)
	{
		// Adjust the clip size.
		start->last = last;

		if (next != start)
		{
			// Remove start+1 to next from the clip list,
			// because start now covers their area.
			for (i = 1, j = newend - next; j > 0; i++, j--)
			{
				start[i] = next[i];
			}
			newend = start+i;
		}
	}
}



//
// R_ClearClipSegs
//
void R_ClearClipSegs (short left, short right)
{
	solidsegs[0].first = -0x7fff;	// new short limit --  killough
	solidsegs[0].last = left;
	solidsegs[1].first = right;
	solidsegs[1].last = 0x7fff;		// new short limit --  killough
	newend = solidsegs+2;
}

//
// killough 3/7/98: Hack floor/ceiling heights for deep water etc.
//
// If player's view height is underneath fake floor, lower the
// drawn ceiling to be just under the floor height, and replace
// the drawn floor and ceiling textures, and light level, with
// the control sector's.
//
// Similar for ceiling, only reflected.
//
// killough 4/11/98, 4/13/98: fix bugs, add 'back' parameter
//

sector_t *R_FakeFlat(sector_t *sec, sector_t *tempsec,
					 int *floorlightlevel, int *ceilinglightlevel,
					 BOOL back)
{
	// [RH] allow per-plane lighting
	if (floorlightlevel != NULL)
	{
		if (sec->FloorFlags & SECF_ABSLIGHTING)
		{
			*floorlightlevel = sec->FloorLight;
		}
		else
		{
			*floorlightlevel = clamp (sec->lightlevel + (SBYTE)sec->FloorLight, 0, 255);
		}
	}

	if (ceilinglightlevel != NULL)
	{
		if (sec->CeilingFlags & SECF_ABSLIGHTING)
		{
			*ceilinglightlevel = sec->CeilingLight;
		}
		else
		{
			*ceilinglightlevel = clamp (sec->lightlevel + (SBYTE)sec->CeilingLight, 0, 255);
		}
	}

	if (sec->heightsec)
	{
		const sector_t *s = sec->heightsec;
		sector_t *heightsec = camera->subsector->sector->heightsec;
		int underwater = heightsec && viewz <= heightsec->floorplane.ZatPoint (viewx, viewy);

		// Replace sector being drawn, with a copy to be hacked
		*tempsec = *sec;

		// Replace floor and ceiling height with other sector's heights.
		tempsec->floorplane    = s->floorplane;
		tempsec->ceilingplane  = s->ceilingplane;

		fixed_t refflorz = s->floorplane.ZatPoint (viewx, viewy);
		fixed_t refceilz = s->ceilingplane.ZatPoint (viewx, viewy);
		fixed_t orgflorz = sec->floorplane.ZatPoint (viewx, viewy);
		fixed_t orgceilz = sec->ceilingplane.ZatPoint (viewx, viewy);

		if (sec->alwaysfake)
		{
		// Code for ZDoom. Allows the effect to be visible outside sectors with
		// fake flat. The original is still around in case it turns out that this
		// isn't always appropriate (which it isn't).
			if (viewz <= refflorz && refflorz > orgflorz)
			{
				tempsec->floorplane			= sec->floorplane;
				tempsec->floorcolormap		= s->floorcolormap;
				tempsec->floorpic			= s->floorpic;
				tempsec->floor_xoffs		= s->floor_xoffs;
				tempsec->floor_yoffs		= s->floor_yoffs;
				tempsec->floor_xscale		= s->floor_xscale;
				tempsec->floor_yscale		= s->floor_yscale;
				tempsec->floor_angle		= s->floor_angle;
				tempsec->base_floor_angle	= s->base_floor_angle;
				tempsec->base_floor_yoffs	= s->base_floor_yoffs;

				tempsec->ceilingplane.ChangeHeight (-1);
				tempsec->ceilingcolormap = s->ceilingcolormap;
				if (s->ceilingpic == skyflatnum)
				{
					tempsec->floorplane			= tempsec->ceilingplane;
					tempsec->floorplane.ChangeHeight (+1);
					tempsec->ceilingpic			= tempsec->floorpic;
					tempsec->ceiling_xoffs		= tempsec->floor_xoffs;
					tempsec->ceiling_yoffs		= tempsec->floor_yoffs;
					tempsec->ceiling_xscale		= tempsec->floor_xscale;
					tempsec->ceiling_yscale		= tempsec->floor_yscale;
					tempsec->ceiling_angle		= tempsec->floor_angle;
					tempsec->base_ceiling_angle = tempsec->base_floor_angle;
					tempsec->base_ceiling_yoffs = tempsec->base_floor_yoffs;
				}
				else
				{
					tempsec->ceilingpic    = s->ceilingpic;
					tempsec->ceiling_xoffs = s->ceiling_xoffs;
					tempsec->ceiling_yoffs = s->ceiling_yoffs;
					tempsec->ceiling_xscale = s->ceiling_xscale;
					tempsec->ceiling_yscale = s->ceiling_yscale;
					tempsec->ceiling_angle = s->ceiling_angle;
					tempsec->base_ceiling_angle = s->base_ceiling_angle;
					tempsec->base_ceiling_yoffs = s->base_ceiling_yoffs;
				}
				tempsec->lightlevel = s->lightlevel;

				if (floorlightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*floorlightlevel = s->FloorLight;
					}
					else
					{
						*floorlightlevel = clamp (s->lightlevel + (SBYTE)s->FloorLight, 0, 255);
					}
				}

				if (ceilinglightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*ceilinglightlevel = s->FloorLight;
					}
					else
					{
						*ceilinglightlevel = clamp (s->lightlevel + (SBYTE)s->CeilingLight, 0, 255);
					}
				}
			}
			else if (viewz > refceilz && refceilz < orgceilz)
			{
				tempsec->ceilingplane		= s->ceilingplane;
				tempsec->floorplane			= s->ceilingplane;
				tempsec->floorplane.ChangeHeight (+1);
				tempsec->ceilingcolormap	= s->ceilingcolormap;
				tempsec->floorcolormap		= s->floorcolormap;

				tempsec->floorpic			= tempsec->ceilingpic    = s->ceilingpic;
				tempsec->floor_xoffs		= tempsec->ceiling_xoffs = s->ceiling_xoffs;
				tempsec->floor_yoffs		= tempsec->ceiling_yoffs = s->ceiling_yoffs;
				tempsec->floor_xscale		= tempsec->ceiling_xscale = s->ceiling_xscale;
				tempsec->floor_yscale		= tempsec->ceiling_yscale = s->ceiling_yscale;
				tempsec->floor_angle		= tempsec->ceiling_angle = s->ceiling_angle;
				tempsec->base_floor_angle	= tempsec->base_ceiling_angle = s->base_ceiling_angle;
				tempsec->base_floor_yoffs	= tempsec->base_ceiling_yoffs = s->base_ceiling_yoffs;

				if (s->floorpic != skyflatnum)
				{
					tempsec->ceilingplane		= sec->ceilingplane;
					tempsec->floorpic			= s->floorpic;
					tempsec->floor_xoffs		= s->floor_xoffs;
					tempsec->floor_yoffs		= s->floor_yoffs;
					tempsec->floor_xscale		= s->floor_xscale;
					tempsec->floor_yscale		= s->floor_yscale;
					tempsec->floor_angle		= s->floor_angle;
					tempsec->base_floor_angle	= s->base_floor_angle;
					tempsec->base_floor_yoffs	= s->base_floor_yoffs;
				}

				tempsec->lightlevel  = s->lightlevel;

				if (floorlightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*floorlightlevel = s->FloorLight;
					}
					else
					{
						*floorlightlevel = clamp (s->lightlevel + (SBYTE)s->FloorLight, 0, 255);
					}
				}

				if (ceilinglightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*ceilinglightlevel = s->FloorLight;
					}
					else
					{
						*ceilinglightlevel = clamp (s->lightlevel + (SBYTE)s->CeilingLight, 0, 255);
					}
				}
			}
		}
		else
		{
		// Original BOOM code
			if ((underwater && (tempsec->  floorplane = sec->floorplane,
								tempsec->ceilingplane = s->floorplane,
								tempsec->ceilingplane.ChangeHeight(-1),
								!back)) || viewz <= refflorz)
			{					// head-below-floor hack
				tempsec->floorpic			= s->floorpic;
				tempsec->floor_xoffs		= s->floor_xoffs;
				tempsec->floor_yoffs		= s->floor_yoffs;
				tempsec->floor_xscale		= s->floor_xscale;
				tempsec->floor_yscale		= s->floor_yscale;
				tempsec->floor_angle		= s->floor_angle;
				tempsec->base_floor_angle	= s->base_floor_angle;
				tempsec->base_floor_yoffs	= s->base_floor_yoffs;

				if (underwater)
				{
					tempsec->lightlevel			= s->lightlevel;
					tempsec->floorcolormap		= s->floorcolormap;
					tempsec->ceilingplane		= s->floorplane;
					tempsec->ceilingplane.ChangeHeight (-1);
					tempsec->ceilingcolormap	= s->ceilingcolormap;
					if (s->ceilingpic == skyflatnum)
					{
						tempsec->floorplane			= tempsec->ceilingplane;
						tempsec->floorplane.ChangeHeight (+1);
						tempsec->ceilingpic			= tempsec->floorpic;
						tempsec->ceiling_xoffs		= tempsec->floor_xoffs;
						tempsec->ceiling_yoffs		= tempsec->floor_yoffs;
						tempsec->ceiling_xscale		= tempsec->floor_xscale;
						tempsec->ceiling_yscale		= tempsec->floor_yscale;
						tempsec->ceiling_angle		= tempsec->floor_angle;
						tempsec->base_ceiling_angle	= tempsec->base_floor_angle;
						tempsec->base_ceiling_yoffs	= tempsec->base_floor_yoffs;
					}
					else
					{
						tempsec->ceilingpic			= s->ceilingpic;
						tempsec->ceiling_xoffs		= s->ceiling_xoffs;
						tempsec->ceiling_yoffs		= s->ceiling_yoffs;
						tempsec->ceiling_xscale		= s->ceiling_xscale;
						tempsec->ceiling_yscale		= s->ceiling_yscale;
						tempsec->ceiling_angle		= s->ceiling_angle;
						tempsec->base_ceiling_angle	= s->base_ceiling_angle;
						tempsec->base_ceiling_yoffs	= s->base_ceiling_yoffs;
					}
				}
				else
				{
					tempsec->floorplane = sec->floorplane;
				}

				if (floorlightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*floorlightlevel = s->FloorLight;
					}
					else
					{
						*floorlightlevel = clamp (s->lightlevel + (SBYTE)s->FloorLight, 0, 255);
					}
				}

				if (ceilinglightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*ceilinglightlevel = s->FloorLight;
					}
					else
					{
						*ceilinglightlevel = clamp (s->lightlevel + (SBYTE)s->CeilingLight, 0, 255);
					}
				}
			}
			else if (heightsec && viewz >= heightsec->ceilingplane.ZatPoint (viewx, viewy) &&
					 orgceilz > refceilz)
			{	// Above-ceiling hack
				tempsec->ceilingplane		= s->ceilingplane;
				tempsec->floorplane			= s->ceilingplane;
				tempsec->floorplane.ChangeHeight (+1);
				tempsec->ceilingcolormap	= s->ceilingcolormap;
				tempsec->floorcolormap		= s->floorcolormap;

				tempsec->floorpic			= tempsec->ceilingpic			= s->ceilingpic;
				tempsec->floor_xoffs		= tempsec->ceiling_xoffs		= s->ceiling_xoffs;
				tempsec->floor_yoffs		= tempsec->ceiling_yoffs		= s->ceiling_yoffs;
				tempsec->floor_xscale		= tempsec->ceiling_xscale		= s->ceiling_xscale;
				tempsec->floor_yscale		= tempsec->ceiling_yscale		= s->ceiling_yscale;
				tempsec->floor_angle		= tempsec->ceiling_angle		= s->ceiling_angle;
				tempsec->base_floor_angle	= tempsec->base_ceiling_angle	= s->base_ceiling_angle;
				tempsec->base_floor_yoffs	= tempsec->base_ceiling_yoffs	= s->base_ceiling_yoffs;

				if (s->floorpic != skyflatnum)
				{
					tempsec->ceilingplane	= sec->ceilingplane;
					tempsec->floorpic		= s->floorpic;
					tempsec->floor_xoffs	= s->floor_xoffs;
					tempsec->floor_yoffs	= s->floor_yoffs;
					tempsec->floor_xscale	= s->floor_xscale;
					tempsec->floor_yscale	= s->floor_yscale;
					tempsec->floor_angle	= s->floor_angle;
				}

				tempsec->lightlevel  = s->lightlevel;

				if (floorlightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*floorlightlevel = s->FloorLight;
					}
					else
					{
						*floorlightlevel = clamp (s->lightlevel + (SBYTE)s->FloorLight, 0, 255);
					}
				}

				if (ceilinglightlevel != NULL)
				{
					if (s->FloorFlags & SECF_ABSLIGHTING)
					{
						*ceilinglightlevel = s->FloorLight;
					}
					else
					{
						*ceilinglightlevel = clamp (s->lightlevel + (SBYTE)s->CeilingLight, 0, 255);
					}
				}
			}
		}
		sec = tempsec;					// Use other sector
	}
	return sec;
}


//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//

void R_AddLine (seg_t *line)
{
	static sector_t tempsec;	// killough 3/8/98: ceiling/water hack
	bool			solid;
	fixed_t			tx1, tx2, ty1, ty2;

	curline = line;

	// [RH] Color if not texturing line
	dc_color = (((line - segs) * 8) + 4) & 255;

	tx1 = line->v1->x - viewx;
	tx2 = line->v2->x - viewx;
	ty1 = line->v1->y - viewy;
	ty2 = line->v2->y - viewy;

	// Reject lines not facing viewer
	if (DMulScale32 (ty1, tx1-tx2, tx1, ty2-ty1) >= 0)
		return;

	WallTX1 = DMulScale20 (tx1, viewsin, -ty1, viewcos);
	WallTX2 = DMulScale20 (tx2, viewsin, -ty2, viewcos);

	WallTY1 = DMulScale20 (tx1, viewtancos, ty1, viewtansin);
	WallTY2 = DMulScale20 (tx2, viewtancos, ty2, viewtansin);

	if (MirrorFlags & RF_XFLIP)
	{
		int t = 256-WallTX1;
		WallTX1 = 256-WallTX2;
		WallTX2 = t;
		swap (WallTY1, WallTY2);
	}

	if (WallTX1 >= -WallTY1)
	{
		if (WallTX1 > WallTY1) return;	// left edge is off the right side
		WallSX1 = (centerxfrac + Scale (WallTX1, centerxfrac, WallTY1)) >> FRACBITS;
		if (WallTX1 >= 0) WallSX1 = MIN (viewwidth, WallSX1+1); // fix for signed divide
		WallSZ1 = WallTY1;
	}
	else
	{
		if (WallTX2 < -WallTY2) return;	// wall is off the left side
		fixed_t den = WallTX1 - WallTX2 - WallTY2 + WallTY1;	
		if (den == 0) return;
		WallSX1 = 0;
		WallSZ1 = WallTY1 + Scale (WallTY2 - WallTY1, WallTX1 + WallTY1, den);
	}

	if (WallSZ1 < 32)
		return;

	if (WallTX2 <= WallTY2)
	{
		if (WallTX2 < -WallTY2) return;	// right edge is off the left side
		WallSX2 = (centerxfrac + Scale (WallTX2, centerxfrac, WallTY2)) >> FRACBITS;
		if (WallTX2 >= 0) WallSX2 = MIN (viewwidth, WallSX2+1);	// fix for signed divide
		WallSZ2 = WallTY2;
	}
	else
	{
		if (WallTX1 > WallTY1) return;	// wall is off the right side
		fixed_t den = WallTY2 - WallTY1 - WallTX2 + WallTX1;
		if (den == 0) return;
		WallSX2 = viewwidth;
		WallSZ2 = WallTY1 + Scale (WallTY2 - WallTY1, WallTX1 - WallTY1, den);
	}

	if (WallSZ2 < 32 || WallSX2 <= WallSX1)
		return;

	if (WallSX1 > WindowRight || WallSX2 < WindowLeft)
		return;

	vertex_t *v1, *v2;

	v1 = line->linedef->v1;
	v2 = line->linedef->v2;

	if ((v1 == line->v1 && v2 == line->v2) || (v2 == line->v1 && v1 == line->v2))
	{
		if (MirrorFlags & RF_XFLIP)
		{
			WallUoverZorg = (float)WallTX2 * WallTMapScale;
			WallUoverZstep = (float)(-WallTY2) * 32.f;
			WallInvZorg = (float)(WallTX2 - WallTX1) * WallTMapScale;
			WallInvZstep = (float)(WallTY1 - WallTY2) * 32.f;
		}
		else
		{
			WallUoverZorg = (float)WallTX1 * WallTMapScale;
			WallUoverZstep = (float)(-WallTY1) * 32.f;
			WallInvZorg = (float)(WallTX1 - WallTX2) * WallTMapScale;
			WallInvZstep = (float)(WallTY2 - WallTY1) * 32.f;
		}
	}
	else
	{
		if (line->linedef->sidenum[0] != line->sidedef - sides)
		{
			swap (v1, v2);
		}
		tx1 = v1->x - viewx;
		tx2 = v2->x - viewx;
		ty1 = v1->y - viewy;
		ty2 = v2->y - viewy;

		fixed_t fullx1 = DMulScale20 (tx1, viewsin, -ty1, viewcos);
		fixed_t fullx2 = DMulScale20 (tx2, viewsin, -ty2, viewcos);
		fixed_t fully1 = DMulScale20 (tx1, viewtancos, ty1, viewtansin);
		fixed_t fully2 = DMulScale20 (tx2, viewtancos, ty2, viewtansin);

		if (MirrorFlags & RF_XFLIP)
		{
			fullx1 = -fullx1;
			fullx2 = -fullx2;
		}

		WallUoverZorg = (float)fullx1 * WallTMapScale;
		WallUoverZstep = (float)(-fully1) * 32.f;
		WallInvZorg = (float)(fullx1 - fullx2) * WallTMapScale;
		WallInvZstep = (float)(fully2 - fully1) * 32.f;
	}
	WallDepthScale = WallInvZstep * WallTMapScale2;
	WallDepthOrg = -WallUoverZstep * WallTMapScale2;

	backsector = line->backsector;

	rw_frontcz1 = frontsector->ceilingplane.ZatPoint (line->v1->x, line->v1->y);
	rw_frontfz1 = frontsector->floorplane.ZatPoint (line->v1->x, line->v1->y);
	rw_frontcz2 = frontsector->ceilingplane.ZatPoint (line->v2->x, line->v2->y);
	rw_frontfz2 = frontsector->floorplane.ZatPoint (line->v2->x, line->v2->y);

	rw_mustmarkfloor = rw_mustmarkceiling = false;

	// Single sided line?
	if (backsector == NULL)
	{
		solid = true;
	}
	else
	{
		// killough 3/8/98, 4/4/98: hack for invisible ceilings / deep water
		backsector = R_FakeFlat (backsector, &tempsec, NULL, NULL, true);

		doorclosed = 0;		// killough 4/16/98

		rw_backcz1 = backsector->ceilingplane.ZatPoint (line->v1->x, line->v1->y);
		rw_backfz1 = backsector->floorplane.ZatPoint (line->v1->x, line->v1->y);
		rw_backcz2 = backsector->ceilingplane.ZatPoint (line->v2->x, line->v2->y);
		rw_backfz2 = backsector->floorplane.ZatPoint (line->v2->x, line->v2->y);

		// Closed door.
		if ((rw_backcz1 <= rw_frontfz1 && rw_backcz2 <= rw_frontfz2) ||
			(rw_backfz1 >= rw_frontcz1 && rw_backfz2 >= rw_frontcz2))
		{
			solid = true;
		}
		else if (
			(backsector->ceilingpic != skyflatnum ||
			frontsector->ceilingpic != skyflatnum)

		// if door is closed because back is shut:
		&& rw_backcz1 <= rw_backfz1 && rw_backcz2 <= rw_backfz2

		// preserve a kind of transparent door/lift special effect:
		&& rw_backcz1 >= rw_frontcz1 && rw_backcz2 >= rw_frontcz2

		&& ((rw_backfz1 <= rw_frontfz1 && rw_backfz2 <= rw_frontfz2) || line->sidedef->bottomtexture != 0))
		{
		// killough 1/18/98 -- This function is used to fix the automap bug which
		// showed lines behind closed doors simply because the door had a dropoff.
		//
		// It assumes that Doom has already ruled out a door being closed because
		// of front-back closure (e.g. front floor is taller than back ceiling).

		// This fixes the automap floor height bug -- killough 1/18/98:
		// killough 4/7/98: optimize: save result in doorclosed for use in r_segs.c
			doorclosed = true;
			solid = true;
		}
		else if (frontsector->ceilingplane != backsector->ceilingplane ||
			frontsector->floorplane != backsector->floorplane)
		{
		// Window.
			solid = false;
		}
		else if (backsector->lightlevel != frontsector->lightlevel
			|| backsector->floorpic != frontsector->floorpic
			|| backsector->ceilingpic != frontsector->ceilingpic
			|| curline->sidedef->midtexture != 0

			// killough 3/7/98: Take flats offsets into account:
			|| backsector->floor_xoffs != frontsector->floor_xoffs
			|| (backsector->floor_yoffs + backsector->base_floor_yoffs) != (frontsector->floor_yoffs + backsector->base_floor_yoffs)
			|| backsector->ceiling_xoffs != frontsector->ceiling_xoffs
			|| (backsector->ceiling_yoffs + backsector->base_ceiling_yoffs) != (frontsector->ceiling_yoffs + frontsector->base_ceiling_yoffs)

			|| backsector->FloorLight != frontsector->FloorLight
			|| backsector->CeilingLight != frontsector->CeilingLight
			|| backsector->FloorFlags != frontsector->FloorFlags
			|| backsector->CeilingFlags != frontsector->CeilingFlags

			// [RH] Also consider colormaps
			|| backsector->floorcolormap != frontsector->floorcolormap
			|| backsector->ceilingcolormap != frontsector->ceilingcolormap

			// [RH] and scaling
			|| backsector->floor_xscale != frontsector->floor_xscale
			|| backsector->floor_yscale != frontsector->floor_yscale
			|| backsector->ceiling_xscale != frontsector->ceiling_xscale
			|| backsector->ceiling_yscale != frontsector->ceiling_yscale

			// [RH] and rotation
			|| (backsector->floor_angle + backsector->base_floor_angle) != (frontsector->floor_angle + frontsector->base_floor_angle)
			|| (backsector->ceiling_angle + backsector->base_ceiling_angle) != (frontsector->ceiling_angle + frontsector->base_ceiling_angle)
			)
		{
			solid = false;
		}
		else
		{
			// Reject empty lines used for triggers and special events.
			// Identical floor and ceiling on both sides, identical light levels
			// on both sides, and no middle texture.
			return;
		}
	}

	rw_prepped = false;
	rw_ceilstat = WallMost (walltop, frontsector->ceilingplane);
	rw_floorstat = WallMost (wallbottom, frontsector->floorplane);

	// [RH] treat off-screen walls as solid
#if 0	// Maybe later...
	if (!solid)
	{
		if (rw_ceilstat == 12 && line->sidedef->toptexture != 0)
		{
			rw_mustmarkceiling = true;
			solid = true;
		}
		if (rw_floorstat == 3 && line->sidedef->bottomtexture != 0)
		{
			rw_mustmarkfloor = true;
			solid = true;
		}
	}
#endif

	rw_havehigh = rw_havelow = false;
	if (backsector != NULL)
	{
		if (rw_frontcz1 > rw_backcz1 || rw_frontcz2 > rw_backcz2)
		{
			rw_havehigh = true;
			WallMost (wallupper, backsector->ceilingplane);
		}
		if (rw_frontfz1 < rw_backfz1 || rw_frontfz2 < rw_backfz2)
		{
			rw_havelow = true;
			WallMost (walllower, backsector->floorplane);
		}

		// Cannot make these walls solid, because it can result in
		// sprite clipping problems for sprites near the wall
	}

	R_ClipWallSegment (WallSX1, WallSX2, solid);
}


//
// R_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true if some part of the bbox might be visible.
//
static const int checkcoord[12][4] = // killough -- static const
{
	{3,0,2,1},
	{3,0,2,0},
	{3,1,2,0},
	{0},
	{2,0,2,1},
	{0,0,0,0},
	{3,1,3,0},
	{0},
	{2,0,3,1},
	{2,1,3,1},
	{2,1,3,0}
};


static BOOL R_CheckBBox (fixed_t *bspcoord)	// killough 1/28/98: static
{
	int 				boxx;
	int 				boxy;
	int 				boxpos;

	fixed_t 			x1, y1, x2, y2;
	fixed_t				rx1, ry1, rx2, ry2;
	fixed_t				sx1, sx2;
	
	cliprange_t*		start;

	// Find the corners of the box
	// that define the edges from current viewpoint.
	if (viewx <= bspcoord[BOXLEFT])
		boxx = 0;
	else if (viewx < bspcoord[BOXRIGHT])
		boxx = 1;
	else
		boxx = 2;

	if (viewy >= bspcoord[BOXTOP])
		boxy = 0;
	else if (viewy > bspcoord[BOXBOTTOM])
		boxy = 1;
	else
		boxy = 2;

	boxpos = (boxy<<2)+boxx;
	if (boxpos == 5)
		return true;

	x1 = bspcoord[checkcoord[boxpos][0]] - viewx;
	y1 = bspcoord[checkcoord[boxpos][1]] - viewy;
	x2 = bspcoord[checkcoord[boxpos][2]] - viewx;
	y2 = bspcoord[checkcoord[boxpos][3]] - viewy;

	// check clip list for an open space

	// Sitting on a line?
	if (DMulScale32 (y1, x1-x2, x1, y2-y1) >= 0)
		return true;

	rx1 = DMulScale20 (x1, viewsin, -y1, viewcos);
	rx2 = DMulScale20 (x2, viewsin, -y2, viewcos);
	ry1 = DMulScale20 (x1, viewtancos, y1, viewtansin);
	ry2 = DMulScale20 (x2, viewtancos, y2, viewtansin);

	if (MirrorFlags & RF_XFLIP)
	{
		int t = 256-rx1;
		rx1 = 256-rx2;
		rx2 = t;
		swap (ry1, ry2);
	}

	if (rx1 >= -ry1)
	{
		if (rx1 > ry1) return false;	// left edge is off the right side
		sx1 = (centerxfrac + Scale (rx1, centerxfrac, ry1)) >> FRACBITS;
		if (rx1 >= 0) sx1 = MIN (viewwidth, sx1+1);	// fix for signed divide
	}
	else
	{
		if (rx2 < -ry2) return false;	// wall is off the left side
		if (rx1 - rx2 - ry2 + ry1 == 0) return false;	// wall does not intersect view volume
		sx1 = 0;
	}

	if (rx2 <= ry2)
	{
		if (rx2 < -ry2) return false;	// right edge is off the left side
		sx2 = (centerxfrac + Scale (rx2, centerxfrac, ry2)) >> FRACBITS;
		if (rx2 >= 0) sx2 = MIN (viewwidth, sx2+1); // fix for signed divide
	}
	else
	{
		if (rx1 > ry1) return false;	// wall is off the right side
		if (ry2 - ry1 - rx2 + rx1 == 0) return false;	// wall does not intersect view volume
		sx2 = viewwidth;
	}

	// Find the first clippost that touches the source post
	//	(adjacent pixels are touching).

	// Does not cross a pixel.
	if (sx2 <= sx1)
		return false;

	start = solidsegs;
	while (start->last < sx2)
		start++;

	if (sx1 >= start->first && sx2 <= start->last)
	{
		// The clippost contains the new span.
		return false;
	}

	return true;
}

//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
void R_Subsector (int num)
{
	int 		 count;
	seg_t*		 line;
	subsector_t *sub;
	sector_t     tempsec;				// killough 3/7/98: deep water hack
	int          floorlightlevel;		// killough 3/16/98: set floor lightlevel
	int          ceilinglightlevel;		// killough 4/11/98

#ifdef RANGECHECK
	if (num>=numsubsectors)
		I_Error ("R_Subsector: ss %i with numss = %i", num, numsubsectors);
#endif

	sub = &subsectors[num];
	frontsector = sub->sector;
	count = sub->numlines;
	line = &segs[sub->firstline];

	// killough 3/8/98, 4/4/98: Deep water / fake ceiling effect
	frontsector = R_FakeFlat(frontsector, &tempsec, &floorlightlevel,
						   &ceilinglightlevel, false);	// killough 4/11/98

	basecolormap = frontsector->ceilingcolormap->Maps;

	ceilingplane = frontsector->ceilingplane.ZatPoint (viewx, viewy) > viewz ||
		frontsector->ceilingpic == skyflatnum ||
		(frontsector->heightsec && frontsector->heightsec->floorpic == skyflatnum) ?
		R_FindPlane(frontsector->ceilingplane,		// killough 3/8/98
					frontsector->ceilingpic == skyflatnum &&  // killough 10/98
						frontsector->sky & PL_SKYFLAT ? frontsector->sky :
						frontsector->ceilingpic,
					ceilinglightlevel,				// killough 4/11/98
					frontsector->ceiling_xoffs,		// killough 3/7/98
					frontsector->ceiling_yoffs + frontsector->base_ceiling_yoffs,
					frontsector->ceiling_xscale,
					frontsector->ceiling_yscale,
					frontsector->ceiling_angle + frontsector->base_ceiling_angle,
					frontsector->SkyBox
					) : NULL;

	basecolormap = frontsector->floorcolormap->Maps;	// [RH] set basecolormap

	// killough 3/7/98: Add (x,y) offsets to flats, add deep water check
	// killough 3/16/98: add floorlightlevel
	// killough 10/98: add support for skies transferred from sidedefs
	floorplane = frontsector->floorplane.ZatPoint (viewx, viewy) < viewz || // killough 3/7/98
		(frontsector->heightsec && frontsector->heightsec->ceilingpic == skyflatnum) ?
		R_FindPlane(frontsector->floorplane,
					frontsector->floorpic == skyflatnum &&  // killough 10/98
						frontsector->sky & PL_SKYFLAT ? frontsector->sky :
						frontsector->floorpic,
					floorlightlevel,				// killough 3/16/98
					frontsector->floor_xoffs,		// killough 3/7/98
					frontsector->floor_yoffs + frontsector->base_floor_yoffs,
					frontsector->floor_xscale,
					frontsector->floor_yscale,
					frontsector->floor_angle + frontsector->base_floor_angle,
					frontsector->SkyBox
					) : NULL;

	// [RH] set foggy flag
	foggy = level.fadeto || frontsector->floorcolormap->Fade
						 || frontsector->ceilingcolormap->Fade;
	r_actualextralight = foggy ? 0 : extralight << 4;

	// killough 9/18/98: Fix underwater slowdown, by passing real sector 
	// instead of fake one. Improve sprite lighting by basing sprite
	// lightlevels on floor & ceiling lightlevels in the surrounding area.
	R_AddSprites (sub->sector, (floorlightlevel + ceilinglightlevel) / 2);

	if (sub->poly)
	{ // Render the polyobj in the subsector first
		int polyCount = sub->poly->numsegs;
		seg_t **polySeg = sub->poly->segs;
		while (polyCount--)
		{
			R_AddLine (*polySeg++);
		}
	}

	while (count--)
	{
		R_AddLine (line++);
	}
}

//
// RenderBSPNode
// Renders all subsectors below a given node, traversing subtree recursively.
// Just call with BSP root.
// killough 5/2/98: reformatted, removed tail recursion

void R_RenderBSPNode (int bspnum)
{
	while (!(bspnum & NF_SUBSECTOR))  // Found a subsector?
	{
		node_t *bsp = &nodes[bspnum];

		// Decide which side the view point is on.
		int side = R_PointOnSide (viewx, viewy, bsp);

		// Recursively divide front space (toward the viewer).
		if (R_CheckBBox (bsp->bbox[side]))
			R_RenderBSPNode (bsp->children[side]);

		// Possibly divide back space (away from the viewer).
		if (!R_CheckBBox (bsp->bbox[side^1]))
			return;

		bspnum = bsp->children[side^1];
	}
	R_Subsector (bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}
