/*
 *  Copyright (C) 2013  inXile entertainment
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __WASTELAND_EXT_H__
#define __WASTELAND_EXT_H__

struct SDL_AudioSpec;
struct SDL_Surface;

namespace WastelandEXT
{
	void Init();
	void Purge();
	
	void InitAudio(SDL_AudioSpec* mixer);
	void UpdateAudio(Bit8u *stream, int len);
	
	bool PreUpdate(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bitu outPitch);
	void Update( SDL_Surface* surface );
	void PostUpdate();

#ifdef DOSBOX_KEYBOARD_H
	bool KeyEvent(KBD_KEYS keytype,bool pressed);
#endif
	bool MouseEvent(int button, bool pressed);
}

#endif