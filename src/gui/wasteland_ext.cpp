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

#include "dosbox.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>

#include "timer.h"
#include "render.h"

#include "mouse.h"
#include "keyboard.h"
#include "shell.h"
#include "cross.h"

#include "wasteland_ext.h"

#include "render_scalers.h"

#include <png.h>

static int MAX_TARGET_W = 0;
static int MAX_TARGET_H = 0;

static int TARGET_W;
static int TARGET_H;

#define CLIP_X ((TARGET_W - 960) / 2)
#define CLIP_Y ((TARGET_H - 600) / 2)

extern void * GFX_GetBlitPix(int& w, int& h);

static int rectCount;
static int rectCountPrev = 0;
static SDL_Rect updateRects[32], updateRectsPrev[32];

static void BeginBlit(int x, int y, int w, int h);

static void Mouse_GetHQ3XCursor(int& x, int& y, int xOffset = 14, int yOffset = 5)
{
	int mx, my;
	Mouse_CursorGet(mx, my);
	x = 3 * (mx + xOffset); y = 3*(my+yOffset);
}

struct GrayImg
{
	int w, h;
	unsigned char pix[1];

	GrayImg* GetNext()
	{
		int pixBufferSize = w*h;
		return reinterpret_cast<GrayImg*>( &pix[pixBufferSize + (4 - pixBufferSize&3 )] );
	}

	void BlitClipped( int X, int Y, int yOffset, int maxH, Bit8u* outWrite, Bitu outPitch )
	{
		Bit8u* dst = outWrite + (Y + CLIP_Y) * outPitch + (X + CLIP_X) * 4;
		unsigned char* src = &pix[ yOffset * w ];

		bool needsFill = h < maxH;
		int blitH = needsFill ? h : maxH;

		BeginBlit(X, Y, w, maxH);

		for( int y = 0; y < blitH; ++y )
		{
			Bit8u* row = dst + y * outPitch;
			for( int x = 0; x < w; ++x )
			{
#ifdef MACOSX
				row[3] = *src;
				row[2] = *src;
				row[1] = *src;
#else
				row[0] = *src;
				row[1] = *src;
				row[2] = *src;
#endif
				row += 4;
				++src;
			}
		}

		if( needsFill )
		{
			for( int y = blitH; y < maxH; ++y )
			{
				Bit8u* row = dst + y * outPitch;
				for( int x = 0; x < w; ++x )
				{
#ifdef MACOSX
					row[3] = row[2] =  row[1] = 0;
#else
					row[0] = row[1] =  row[2] = 0;
#endif
					row += 4;
					++src;
				}
			}
		}
	}

	bool Compare( Bit8u* start, Bit8u* pal, Bitu pitch, Bit32u maxMismatchedPix = 0 )
	{
		unsigned char* pPix = pix;
		Bit32u misMatchedPixels = 0;
		for( int y = 0; y < h; ++y )
		{
			Bit8u* row = start;
			for( int x = 0; x < w; ++x )
			{
				Bit8u* srcPix = pal + *row * 4;

				if( srcPix[0] != *pPix || srcPix[1] != *pPix || srcPix[2] != *pPix )
				{
					if( ++misMatchedPixels >= maxMismatchedPix )
					{
						return false;
					}
				}
				++pPix;
				++row;
			}
			start += pitch;
		}
		return true;
	}

	bool CompareText( Bit8u* start, Bit8u* pal, Bitu pitch )
	{
		const int charW = 8;

		int numChars = w / charW;
		int numMisMatchedChars = 0;
		
		for( int c = 0; c < numChars; ++c )
		{
			Bit8u* col = start + c * charW;
			unsigned char* pCol = pix + c * charW;
			bool matches = true;
			for( int y = 0; matches && y < h; ++y )
			{
				Bit8u* row = col;
				unsigned char* pPix = pCol;
				for( int x = 0; matches && x < charW; ++x )
				{
					Bit8u* srcPix = pal + *row * 4;

					if( srcPix[0] != *pPix || srcPix[1] != *pPix || srcPix[2] != *pPix )
					{
						numMisMatchedChars++;
						matches = false;
					}
					++pPix;
					++row;
				}
				col += pitch;
				pCol += w;
			}
		}
		return numMisMatchedChars <= 3; //mouse can overlap 3 characters.
	}

	bool CompareClipped( Bit8u* start, Bit8u* pal, Bitu pitch, int yOffset, int maxH, Bit32u maxMismatchedPix = 0 )
	{
		unsigned char* pPix = &pix[yOffset*w];
		Bit32u misMatchedPixels = 0;
		for( int y = 0; y < maxH; ++y )
		{
			Bit8u* row = start;
			for( int x = 0; x < w; ++x )
			{
				Bit8u* srcPix = pal + *row * 4;

				if( srcPix[0] != *pPix || srcPix[1] != *pPix || srcPix[2] != *pPix )
				{
					if( ++misMatchedPixels >= maxMismatchedPix )
					{
						return false;
					}
				}
				++pPix;
				++row;
			}
			start += pitch;
		}
		return true;
	}
};

struct RGBAPix
{
	inline unsigned char Saturate(float c)
	{
		return c < 0 ? 0 : c > 255 ? 255 : static_cast<unsigned char>(c);
	}

	inline void Blend(Bit8u* row)
	{
		float lerp = float(a) / 255.0f;
		float invLerp = 1.0f - lerp;

#ifdef MACOSX
		row[3] = Saturate(lerp*b + invLerp*row[3]);
		row[2] = Saturate(lerp*g + invLerp*row[2]);
		row[1] = Saturate(lerp*r + invLerp*row[1]);
#else
		row[0] = Saturate(lerp*b + invLerp*row[0]);
		row[1] = Saturate(lerp*g + invLerp*row[1]);
		row[2] = Saturate(lerp*r + invLerp*row[2]);
#endif
	}

	unsigned char r, g, b, a;
};

#ifndef min
#define min(a,b)		(((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b)		(((a) > (b)) ? (a) : (b))
#endif

struct Cursors
{
	enum { COUNT = 8 };

	struct Pix { unsigned char r, g, b, a; };
	struct LD
	{
		enum
		{
			DIM = 16
		};

		Pix pix[DIM*DIM];
	};
	LD ld[COUNT];

	template <typename RECT>
	static int GetMaxOverlapArea(RECT* p)
	{
		return min(p->w, LD::DIM) * min(p->h, LD::DIM);
	}
	struct HD
	{
		enum
		{
			DIM = LD::DIM * 3
		};
		typedef RGBAPix Pix;
		Pix pix[DIM*DIM];
	};
	HD hd[COUNT];

	void CheckForOverlap(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch, Bits origX, Bits origY, Bitu origW, Bitu origH, Bitu cursor = 0)
	{
		int mx, my;
		Mouse_CursorGet(mx, my);

		//cursor is on top left of sprite
		if( mx >= origX - LD::DIM && mx < origX + (Bits)origW &&
			my >= origY - LD::DIM && my < origY + (Bits)origH )
		{
			//just assume it's cursor 0

			Bits blitX = (Bits)mx, blitY = (Bits)my;
			Bitu blitW = LD::DIM, blitH = LD::DIM;

			if( blitX < origX ) { blitW -= (origX - blitX); blitX = origX; }
			if( blitY < origY ) { blitH -= (origY - blitY); blitY = origY; }

			if( (blitX + blitW) > (origX + origW) ) { blitW = (origX + origW) - blitX; }
			if( (blitY + blitH) > (origY + origH) ) { blitH = (origY + origH) - blitY; }

			int lookupOffsetX = 3 * ( blitX - (int)mx );
			int lookupOffsetY = 3 * ( blitY - (int)my );

			blitX *= 3; blitY *= 3; blitW *= 3; blitH *= 3;

			HD::Pix* src = &hd[cursor].pix[lookupOffsetX + HD::DIM * lookupOffsetY];
			Bit8u* dst = outWrite + (blitY + CLIP_Y) * outPitch + (blitX + CLIP_X) * 4;
			for( Bitu y = 0; y < blitH; ++y )
			{
				HD::Pix* srcrow = src;
				Bit8u* dstrow = dst;
				for( Bitu x = 0; x < blitW; ++x )
				{
					srcrow->Blend(dstrow);
					srcrow++;
					dstrow += 4;
				}
				dst += outPitch;
				src += HD::DIM;
			}
		}
	}
};

struct Portraits
{
	enum { DISK1 = 33, DISK2 = 49, COUNT = DISK1 + DISK2 }; // 33 in disk 1 + 49 in disk 2

	struct Pix { unsigned char r, g, b; };
	struct LD
	{
		enum
		{
			X = 8, Y = 8,
			W = 96, H = 84
		};
		LD() {}
		LD(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal);

		float Compare( const LD& other ) const;
		void Dump( const char* filename );

		Pix pix[W*H];

		bool IsNotBlack() const
		{
			const Pix* pPix = pix;
			int nonBlackCount = 0;
			for( int i = 0; i < W*H; ++i )
			{
				if( pPix->r || pPix->g || pPix->b )
				{
					nonBlackCount++;
					if( nonBlackCount >= Cursors::LD::DIM*Cursors::LD::DIM )
					{
						return true;
					}
				}
				++pPix;
			}
			return false;
		}
	};

	struct HD
	{
		enum
		{
			X = LD::X * 3, Y = LD::Y * 3,
			W = LD::W * 3, H = LD::H * 3
		};
		Pix pix[W*H];

		void Blit( Bit8u* outWrite, Bitu outPitch );
	};

	struct Map
	{
		LD ld;
		HD hd;
	};

	Map map[COUNT];
	GrayImg* frameRight;
	GrayImg* frameBottom;

	Portraits()
	{
		frameRight = (GrayImg*)(this + 1);
		frameBottom = frameRight->GetNext();
	}
	void Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch);

	void LoadOverride(int disk, int portrait)
	{
		Pix* dstPix = map[portrait + (disk == 2 ? DISK1 : 0)].hd.pix;
		char overrideName[64];
		snprintf(overrideName, sizeof(overrideName), "portraits/d%dp%03d.png", disk, portrait);
		FILE* f = fopen(overrideName, "rb");
		if( f )
		{
			png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
			png_init_io(png_ptr, f);

			png_infop info_ptr = png_create_info_struct(png_ptr);
			png_read_info(png_ptr, info_ptr);

			png_uint_32 width = 0;
			png_uint_32 height = 0;
			int bitDepth = 0;
			int colorType = -1;
			png_uint_32 retval =
			png_get_IHDR(png_ptr, info_ptr,
					  &width,
					  &height,
					  &bitDepth,
					  &colorType,
					  NULL, NULL, NULL);
			if( width == HD::W &&
				height == HD::H )
			{
				png_uint_32 bytesPerRow = png_get_rowbytes(png_ptr, info_ptr);
				if( colorType == PNG_COLOR_TYPE_PALETTE )
				{
					png_set_palette_to_rgb(png_ptr);
					colorType = PNG_COLOR_TYPE_RGB;
					bytesPerRow = 3 * width;
					// Add full alpha channel if there's transparency
					if(png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
					{
						png_set_tRNS_to_alpha(png_ptr);
						colorType = PNG_COLOR_TYPE_RGBA;
						bytesPerRow += width;
					}
				}
		
				Bit8u* rowData = new Bit8u[bytesPerRow];
        
				switch(colorType)
				{
					case PNG_COLOR_TYPE_RGB:
					{
						for(Bit32u rowIdx = 0; rowIdx < height; ++rowIdx)
						{
							png_read_row(png_ptr, (png_bytep)rowData, NULL);
					
							Pix* pDestRow = dstPix + rowIdx * width;
							Bit8u* pSrcRow = rowData;
					
							for(Bit32u colIdx = 0; colIdx < width; ++colIdx)
							{
								pDestRow->r = *(pSrcRow++);
								pDestRow->g = *(pSrcRow++);
								pDestRow->b = *(pSrcRow++);
								pDestRow++;
							}					
						}
					}
					break;
				
					case PNG_COLOR_TYPE_RGB_ALPHA:
						// read single row at a time
						for(Bit32u rowIdx = 0; rowIdx < height; ++rowIdx)
						{
							png_read_row(png_ptr, (png_bytep)rowData, NULL);
						
							Pix* pDestRow = dstPix + rowIdx * width;
							Bit8u* pSrcRow = rowData;
						
							for(Bit32u colIdx = 0; colIdx < width; ++colIdx)
							{
								pDestRow->r = *(pSrcRow++);
								pDestRow->g = *(pSrcRow++);
								pDestRow->b = *(pSrcRow++);
								pDestRow++; pSrcRow++;							
							}
						}
					break;
				
					case PNG_COLOR_TYPE_GRAY_ALPHA:
						for(Bit32u rowIdx = 0; rowIdx < height; ++rowIdx)
						{
							png_read_row(png_ptr, (png_bytep)rowData, NULL);

							Pix* pDestRow = dstPix + rowIdx * width;
							Bit8u* pSrcRow = rowData;

							for(Bit32u colIdx = 0; colIdx < width; ++colIdx)
							{
								pDestRow->r =
								pDestRow->g =
								pDestRow->b = *(pSrcRow++);
								pDestRow++; pSrcRow++;							
							}
						}
					break;
				
					case PNG_COLOR_TYPE_GRAY:
						if( bitDepth < 8 )
						{
							png_set_gray_1_2_4_to_8(png_ptr);
						}
				
						// read single row at a time
						for(Bit32u rowIdx = 0; rowIdx < height; ++rowIdx)
						{
							png_read_row(png_ptr, (png_bytep)rowData, NULL);

							Pix* pDestRow = dstPix + rowIdx * width;
							Bit8u* pSrcRow = rowData;

							for(Bit32u colIdx = 0; colIdx < width; ++colIdx)
							{
								pDestRow->r =
								pDestRow->g =
								pDestRow->b = *(pSrcRow++);
								pDestRow++;							
							}
						}
					break;
				
					default:;
				}
				delete [] rowData;
			}

			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			fclose(f);
		}
	}

	void LoadOverrides()
	{
		for( int i = 0; i < DISK1; ++i )
		{
			LoadOverride(1, i);
		}
		for( int i = 0; i < DISK2; ++i )
		{
			LoadOverride(2, i);
		}
	}
};


struct Paragraphs
{
	enum { SCR_W = 320, SCR_H = 200 };
	enum { SCAN_X = 8, SCAN_Y = 144 };

	struct RGBAImg
	{
		int w, h;
		typedef RGBAPix Pix;
		Pix pix[1];

		RGBAImg* GetNext()
		{
			int pixBufferSize = w*h;
			return reinterpret_cast<RGBAImg*>( &pix[pixBufferSize] );
		}


		void Blit( int X, int Y, Bit8u* outWrite, Bitu outPitch )
		{
			BeginBlit(X, Y, w, h);

			Bit8u* dst = outWrite + (Y + CLIP_Y) * outPitch + (X + CLIP_X) * 4;
			Pix* src = pix;

			for( int y = 0; y < h; ++y )
			{
				Bit8u* row = dst + y * outPitch;
				for( int x = 0; x < w; ++x )
				{
					src->Blend(row);
					row += 4;
					++src;
				}
			}
		}
	};

	enum
	{
		NUMBER = 10,
		PARAGRAPH = 163, //0 based 162
		MANUAL = 2
	};

	GrayImg* ref;
	GrayImg* numbers[NUMBER];
	GrayImg* charRef[MANUAL];
	GrayImg* paragraphs[PARAGRAPH];
	GrayImg* manual[MANUAL];
	enum
	{
		NO_HIGH,
		HIGH,
		ICON_COUNT
	};
	
	enum
	{
		TOP,
		LEFT,
		RIGHT,
		BOTTOM,
		BOTTOM_HI,
		FRAME_SIDE_COUNT
	};
	Paragraphs();

	RGBAImg* frame[FRAME_SIDE_COUNT];

	enum
	{
		DOWN,
		DOWN_HI,
		UP,
		UP_HI,
		BUTTON_COUNT
	};
	RGBAImg* buttons[BUTTON_COUNT];

	RGBAImg* question[ICON_COUNT]; // highlight state

	void Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch);
	void DisplayBook(int paragraphX, int paragraphY, int mx, int my, Bit8u* lineP, Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch, int startLineOffset);

	static inline void Multiply(Bit8u* row, float r, float g, float b)
	{
#ifdef MACOSX
		row[3] *= b;
		row[2] *= g;
		row[1] *= r;
#else
		row[0] *= b;
		row[1] *= g;
		row[2] *= r;
#endif
	}
	static inline void Set(Bit8u* row, int r, int g, int b)
	{
#ifdef MACOSX
		row[3] = b;
		row[2] = g;
		row[1] = r;
#else
		row[0] = b;
		row[1] = g;
		row[2] = r;
#endif
	}
	
	void MultiplyAndUnderline(Bit8u *outWrite, Bitu outPitch, int x, int y, int w, int h, int r, int g, int b )
	{
		BeginBlit(x, y, w, h + 3);

		Bit8u* dst = outWrite + (y + CLIP_Y) * outPitch + (x + CLIP_X) * 4;
		
		float fR = float(r) / 255.0f;
		float fG = float(g) / 255.0f;
		float fB = float(b) / 255.0f;
		for( int y = 0; y < h; ++y )
		{
			Bit8u* row = dst + y * outPitch;
			for( int x = 0; x < w; ++x )
			{
				Multiply(row, fR, fG, fB);
				row += 4;
			}
		}

		for( int y = h+1; y < h+3; ++y )
		{
			Bit8u* row = dst + y * outPitch;
			for( int x = 0; x < w; ++x )
			{
				Set(row, r, g, b);
				row += 4;
			}
		}
	}
};

Paragraphs::Paragraphs()
{
	FILE* f = fopen("paragraphs.bin", "rb");
	fseek( f, 0, SEEK_END );
	size_t dataSize = ftell(f);
	void* pData = malloc( dataSize );
	fseek( f, 0, SEEK_SET );
	fread( pData, 1, dataSize, f );
	fclose( f );

	{
		GrayImg* pPrev = ref = static_cast<GrayImg*>(pData);
		GrayImg** pCurr = reinterpret_cast<GrayImg**>( &numbers );

		for( int i = 0; i < MANUAL+PARAGRAPH+NUMBER+MANUAL; ++i )
		{
			*pCurr = pPrev->GetNext();
			pPrev=  *pCurr;
			pCurr++;
		}

		frame[0] = reinterpret_cast<RGBAImg*>( pPrev->GetNext() );
	}

	{
		RGBAImg* pPrev = frame[0];
		RGBAImg** pCurr = reinterpret_cast<RGBAImg**>( &frame[1] );

		for( int i = 0; i < FRAME_SIDE_COUNT+BUTTON_COUNT+ICON_COUNT-1; ++i )
		{
			*pCurr = pPrev->GetNext();
			pPrev=  *pCurr;
			pCurr++;
		}
	}

	for( int i = 1; i < PARAGRAPH+MANUAL; ++i )
	{
		assert( paragraphs[i]->w == paragraphs[1]->w );
	}
	assert(frame[LEFT]->h == frame[RIGHT]->h );
	assert(frame[TOP]->w == frame[BOTTOM]->w );
	assert(frame[TOP]->w - frame[LEFT]->w - frame[RIGHT]->w == paragraphs[1]->w );
}

struct Legal
{
	enum
	{
		LEGAL1,
		LEGAL2,
		COUNT
	};

	enum
	{
		REF_X = 72,
	};
	GrayImg* ref[COUNT];
	GrayImg* nrm[COUNT];
	GrayImg* hq[COUNT];

	Legal()
	{
		FILE* f = fopen("legal.bin", "rb");
		fseek( f, 0, SEEK_END );
		size_t dataSize = ftell(f);
		void* pData = malloc( dataSize );
		fseek( f, 0, SEEK_SET );
		fread( pData, 1, dataSize, f );
		fclose( f );

		GrayImg* pPrev = ref[LEGAL1] = static_cast<GrayImg*>(pData);
		GrayImg** pCurr = reinterpret_cast<GrayImg**>( &ref[LEGAL2] );

		for( int i = 0; i < COUNT+COUNT+COUNT-1; ++i )
		{
			*pCurr = pPrev->GetNext();
			pPrev=  *pCurr;
			pCurr++;
		}
	}

	enum { BLIT_X = 8*3, BLIT_Y = Paragraphs::SCAN_Y*3 };

	void Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch);
};

static Legal* spLegal;
static Portraits* spPortraits;
static Paragraphs* spParagraphs;
static Cursors* spCursors;
static void* sPixelCache;
static Bit8u* sPixels = NULL;

#include "stream_ogg.h"

static OGG_stream *music, *voiceover;

static void OGG_safedestroy(OGG_stream*& stream)
{
	if( stream )
	{
		OGG_stop(stream);
		OGG_delete(stream);
		stream = NULL;
	}
}

void WastelandEXT::InitAudio(SDL_AudioSpec *mixer)
{
	OGG_init(mixer);
}

void WastelandEXT::UpdateAudio(Bit8u* stream, int len)
{
	if( music )
	{
		OGG_playAudio(music, stream, len);
	}

	if( voiceover )
	{
		OGG_playAudio(voiceover, stream, len);
	}
}

static void BeginBlit(int x, int y, int w, int h)
{
	if( rectCount == 0 )
	{
		memcpy( sPixels, sPixelCache, TARGET_W*TARGET_H*4 );
	}

	SDL_Rect* pNew = &updateRects[rectCount++];
	pNew->x = x + CLIP_X;
	pNew->y = y + CLIP_Y;
	pNew->w = w;
	pNew->h = h;
	//Scaler_ChangedLines[0] = CLIP_Y;
	//Scaler_ChangedLines[1] = TARGET_H - CLIP_Y;
}

static AutoexecObject* sMountSave;

#ifdef WIN32

#include <Windows.h>

/*function... might want it in some class?*/
static int getdir (std::string dir, std::vector<std::string> &files)
{
	WIN32_FIND_DATA ffd;
	HANDLE hFind = FindFirstFile((dir + "*").c_str(), &ffd);
	do {
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if( strcmp(ffd.cFileName, ".") && strcmp(ffd.cFileName, "..") ) {
				files.push_back(std::string(ffd.cFileName));
			}
		}
	} while (FindNextFile(hFind, &ffd) != 0);
	return 0;
}
#else

#ifdef MACOSX
#include <CoreFoundation/CoreFoundation.h>
#include <sys/param.h> /* for MAXPATHLEN */
#endif

#include <dirent.h>

/*function... might want it in some class?*/
static int getdir (std::string dir, std::vector<std::string> &files)
{
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(dir.c_str())) == NULL) {
        exit(-1);
    }

    while ((dirp = readdir(dp)) != NULL) {
		if( dirp->d_type == DT_DIR &&
			strcmp(dirp->d_name, ".") &&
			strcmp(dirp->d_name, "..") ) {
			files.push_back(std::string(dirp->d_name));
		}
    }
    closedir(dp);
    return 0;
}
#endif

#define MENU_SEPARATOR "echo +---------------------------------------------------------------+\n"

static void WriteMenuLine(std::string& s, const char* val, int lenOffset = 0 )
{
	char menuLineBuf[128] = "W:\\ekko $B                                                               ";

	char* pEnd = &menuLineBuf[strlen(menuLineBuf)];
	for( int i = 0; i < lenOffset; ++i )
	{
		pEnd[0] = ' ';
		pEnd[1] = 0;
		++pEnd;
	}
	strcat(menuLineBuf, "$B\n");

	memcpy( &menuLineBuf[12], val, strlen(val) );
	s += menuLineBuf;
}

#define SETTINGS_FILENAME "SETTINGS.1"
//#define SHIFT_VOLUME_MOD

#ifdef SHIFT_VOLUME_MOD
#define VOICE_INSTR "       "
#define MUSIC_INSTR "       "
#else
#define VOICE_INSTR "(G+/V-)"
#define MUSIC_INSTR "(K+/M-)"
#endif
static bool sSettingsLoaded = false;
enum Settings
{
	PORTRAITS,
	SMOOTHING,
	NUM_TOGGLES,
	VOICE = NUM_TOGGLES,
	MUSIC,
	NUM_VOLUME,
	SOUNDTRACK_MODE = NUM_VOLUME,
	SOUNDTRACK_BASE,
	NUM_SET
};
static const char* sSettingsNames[NUM_SET] = { "PORTRAIT_SETTING", "SMOOTH_SETTING", "VOICE_SETTING", "MUSIC_SETTING", "SOUNDTRACK_MODE_SETTING", "SOUNDTRACK_BASE_SETTING" };
static const char* sSettingsDisp[NUM_SET] =
{
	"    1 - High-Definition portraits     %s",
	"    2 - Smoothing                     %s",
	"    3 - Voice-Over Volume    " VOICE_INSTR "   %s    (min=0 max=9)",
	"    4 - Music Volume         " MUSIC_INSTR "   %s    (min=0 max=9)",
	"    5 - Soundtrack Mode      %s",
	"    6 - Base Track           %s",
};

static void GenBinaryToggle(std::string& autoexec, int i)
{
	autoexec += ":toggle";
	autoexec += sSettingsNames[i];
	autoexec += "\n";
	autoexec += "if \"%";
	autoexec += sSettingsNames[i];
	autoexec += "%\"==\"1\" goto clear";
	autoexec += sSettingsNames[i];
	autoexec += "\n"
				"set ";
	autoexec += sSettingsNames[i];
	autoexec += "=1\n"
				"goto settings\n";
	autoexec += ":clear";
	autoexec += sSettingsNames[i];
	autoexec += "\n"
				"set ";
	autoexec += sSettingsNames[i];
	autoexec += "=0\n"
				"goto settings\n";
}

static const int sSettingsBase[NUM_SET] = { '0', '0', '0', '0', '0', 'a' };

static const char* sTrackNames[] =
{
	"Wasteland Theme",
	"Highpool",
	"Desert",
	"Agricultural Center",
	"Mine Shaft",
	"Desert Nomads",
	"Quartz",
	"Needles",
	"Temple of Blood",
	"Las Vegas",
	"Sleeper Base",
	"Darwin",
	"Savage Village",
	"Mind Maze",
	"Guardian Citadel",
	"Base Chochise",
	"Ranger Center",
	"The Ballad of Faran Brygo"
};

static const int NUM_TRACKS = sizeof(sTrackNames) / sizeof(sTrackNames[0]);
static int sSettings[NUM_SET] = { 1, 1, 9, 9, 0, 0 };

static void LoadSettings(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");

	if( f )
	{
		char settings[1+NUM_SET];
		if( fread( settings, 1, sizeof(settings), f ) == sizeof(settings) )
		{
			for( int i = 0; i < NUM_SET; ++i )
			{
				sSettings[i] = settings[i] - sSettingsBase[i];
			}
		}
		fclose(f);
	}
}

static void InitSetting(std::string& autoexec, Settings s)
{
	char setter[64];
	snprintf( setter, sizeof(setter), "set %s=%c\n", sSettingsNames[s], sSettings[s] + sSettingsBase[s] );
	autoexec += setter;
}

static size_t SetupGameSelectSubmenu(std::string& autoexec, const std::vector<std::string>& saves, const char* startLabel, const char* title, const char* question, const char* targetLabel, void (*titleMore)(std::string&) = NULL)
{
	const char* allSaveChoices = "123456789abcdefghijklmnop";
	size_t MAX_SAVES = strlen(allSaveChoices);
	autoexec += ":";
	autoexec += startLabel;
	autoexec += "\n"
				"cls\n"
				MENU_SEPARATOR;
	WriteMenuLine(autoexec, title);
	if( titleMore )
	{
		titleMore(autoexec);
	}
	autoexec += MENU_SEPARATOR;
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, question);
	WriteMenuLine(autoexec, "");

	size_t saveCount = saves.size() > MAX_SAVES ? MAX_SAVES : saves.size();

	for( size_t i = 0; i < saveCount; ++i )
	{
		char saveoption[256];
		snprintf(saveoption, sizeof(saveoption), "    %d - %s", i+1, saves[i].c_str());
		WriteMenuLine(autoexec, saveoption);
	}
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    q - Return to main menu");
	WriteMenuLine(autoexec, "");
	autoexec += MENU_SEPARATOR
				"echo.\n"
				"choice /n /c";

	char saveChoices[32];
	snprintf(saveChoices, 1+saveCount, allSaveChoices);
	saveChoices[saveCount] = 0;
	autoexec += saveChoices;
	autoexec += "q Your choice? \n";

	char response[64];
	snprintf(response, sizeof(response), "if errorlevel %d goto mainmenu\n", saveCount + 1);
	autoexec += response;

	for( size_t i = saveCount; i != 0; --i )
	{
		snprintf(response, sizeof(response), "if errorlevel %d goto %s%d\n", i, targetLabel, i);
		autoexec += response;
	}
	return saveCount;
}

static void UseWLResetAtOwnRisk(std::string& autoexec)
{
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "!! USE AT YOUR OWN RISK -- NOT OFFICIALLY SUPPORTED !!");
	WriteMenuLine(autoexec, "INCLUDED FOR YOUR CONVENIENCE");
}

void WastelandEXT::Init()
{
#ifdef MACOSX
	char parentdir[MAXPATHLEN];
	CFURLRef url = CFBundleCopyBundleURL(CFBundleGetMainBundle());
	if (CFURLGetFileSystemRepresentation(url, 1, (UInt8 *)parentdir, MAXPATHLEN)) {
		chdir(parentdir);   /* chdir to the binary app's parent */
	}
	CFRelease(url);
#endif
	sMountSave = new AutoexecObject;
	std::string path;
	Cross::CreatePlatformConfigDir(path);

	std::string config_file = path + "/wl.conf";
#ifdef WIN32
	CopyFile( "wl.default.conf", config_file.c_str(), TRUE );
#else
	char cp[256];
	snprintf( cp, sizeof(cp), "cp -n wl.default.conf \"%s\"", config_file.c_str() );
	system( cp );
#endif
	LoadSettings(path + "/" SETTINGS_FILENAME );
	std::string mount = "mount c ";
	std::string autoexec = "@echo off\n";
	autoexec += mount + "\"" + path + "\"\n";

	autoexec += "mount x .\n"
				"mount w ./rom\n"
				"C:\n";
//				"echo Welcome to Wasteland!\n"
//				"echo.\n"
//				"echo (Legal Text Blah blah... GPL blah blah...)\n"
//				"echo.\n";

	for( int s = 0; s < NUM_SET; ++s )
	{
		InitSetting(autoexec, (Settings)s);
	}
//	autoexec += "pause\n";

	autoexec += ":mainmenu\n"
				"cls\n"
				MENU_SEPARATOR;
	WriteMenuLine(autoexec, "MAIN MENU");
	autoexec += MENU_SEPARATOR;

	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "What would you like to do?");
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    n - New game");

	char settingsOption[2] = { '2', '\0' };
	char aboutOption[2] = { '3', '\0' };
	char exitOption[2] = { '4', '\0' };
	char continueOption[2] = { '\0', '\0' };
	bool hasContinueOption = false;
	bool hasResumeOption = false;

	std::vector<std::string> saves;
	getdir(path, saves);

	char lastSave[256];
	snprintf(lastSave, sizeof(lastSave), "%s/LASTSAVE", path.c_str());
	FILE* fp = fopen(lastSave, "r");
	if( fp )
	{
		if( fscanf(fp, "%s", lastSave) == 1 )
		{
			for( size_t i = 0; i < saves.size(); ++i )
			{
				if( strcasecmp( lastSave, saves[i].c_str() ) == 0 )
				{
					settingsOption[0]++;
					exitOption[0]++;
					aboutOption[0]++;
					hasResumeOption = true;
					break;
				}
			}
		}
		fclose(fp);
	}

	if( hasResumeOption )
	{
		char resumeLast[64];
		snprintf(resumeLast, sizeof(resumeLast), "    c - Continue last game (%s)", lastSave);
		WriteMenuLine(autoexec, resumeLast);
	}

	if( saves.size() )
	{
		char continueOp[64];
		continueOption[0] = settingsOption[0];
		aboutOption[0] += 2;
		settingsOption[0] += 2;
		exitOption[0] += 2;
		snprintf(continueOp, sizeof(continueOp), "    l - Load other game", continueOption);
		WriteMenuLine(autoexec, continueOp);
		hasContinueOption = true;

		WriteMenuLine(autoexec, "");
		WriteMenuLine(autoexec, "    r - Unofficial reset utility (wlreset)");
	}

	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    s - Settings");
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    a - About");
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    q - Quit");
	WriteMenuLine(autoexec, "");
	autoexec += MENU_SEPARATOR;

	autoexec += "echo.\n"
				"choice /cn";
	if( hasResumeOption )
	{
		autoexec += "c";
	}
	if( hasContinueOption )
	{
		autoexec += "lr";
	}
	autoexec += "saq Your choice\n";

	autoexec += "if errorlevel ";
	autoexec += exitOption;
	autoexec += " goto leavegame\n";

	autoexec += "if errorlevel ";
	autoexec += aboutOption;
	autoexec += " goto about\n";

	autoexec += "if errorlevel ";
	autoexec += settingsOption;
	autoexec += " goto settings\n";

	if( hasContinueOption )
	{
		char resetOption[2] = { continueOption[0]+1, '\0' };
		autoexec += "if errorlevel ";
		autoexec += resetOption;
		autoexec += " goto wlreset\n";

		autoexec += "if errorlevel ";
		autoexec += continueOption;
		autoexec += " goto selectgame\n";
	}
	if( hasResumeOption )
	{
		autoexec += "if errorlevel 2 goto resumegame\n";
	}
	autoexec += "if errorlevel 1 goto newgame\n";

	if( hasResumeOption )
	{
		autoexec += ":resumegame\n"
					"set NEWGAME=";
		autoexec += lastSave;
		autoexec += "\n"
					"goto launchgame\n";
	}
	autoexec += ":newgame\n"
				"echo.\n"
				"echo.\n"
				"echo Pick a simple name consisting of alphanumeric characters (2-8 in length):\n"
				"echo Enter q to cancel\n"
				"echo.\n"
				"W:\\readkb NEWGAME=line\n"
				"echo.\n"
				"if \"%NEWGAME%\"==\"q\" goto mainmenu\n"
				"W:\\strlen \"%NEWGAME%\"\n"
				"if errorlevel 9 goto newgametoolong\n"
				"if errorlevel 2 goto newgamevalidlen\n"
				"echo Name too short!!\n"
				"goto newgame\n"
				":newgametoolong\n"
				"echo Name too long!!\n"
				"goto newgame\n"
				":newgamevalidlen\n"
				"W:\\alphanum \"%NEWGAME%\"\n"
				"if errorlevel 1 goto newgamevalidcontents\n"
				"echo Name contains invalid characters!!\n"
				"goto newgame\n"
				":newgamevalidcontents\n"
				"if exist %NEWGAME% goto newgameoverwrite\n"
				"goto newgamecreate\n"
				":newgameoverwrite\n"
				"echo Save game %NEWGAME% already exists!!\n"
				"choice Overwrite?\n"
				"if errorlevel 2 goto newgame\n"
				":newgamecreate\n"
				"mkdir %NEWGAME%\n"
				"copy W:\\data\\*.* %NEWGAME% >NUL\n"
				//for wlreset.
				"copy %NEWGAME%\\GAME1 %NEWGAME%\\MASTER1 >NUL\n"
				"copy %NEWGAME%\\GAME2 %NEWGAME%\\MASTER2 >NUL\n"
				"goto launchgame\n";

	autoexec += ":settings\n"
				"cls\n"
				MENU_SEPARATOR;
	WriteMenuLine(autoexec, "SETTINGS MENU");
	autoexec += MENU_SEPARATOR;
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "Select the setting you wish to toggle");
	WriteMenuLine(autoexec, "");

	for( int s = 0; s < NUM_TOGGLES; ++s )
	{
		char dispCondition[128];

		snprintf(dispCondition, sizeof(dispCondition), "if \"%%%s%%\"==\"1\" goto dispon%s\n", sSettingsNames[s], sSettingsNames[s]);
		autoexec += dispCondition;
		snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], "OFF");
		WriteMenuLine(autoexec, dispCondition);
		snprintf(dispCondition, sizeof(dispCondition),	"goto dispdone%s\n"
														":dispon%s\n", sSettingsNames[s], sSettingsNames[s]);
		autoexec += dispCondition;

		snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], "ON");
		WriteMenuLine(autoexec, dispCondition);

		snprintf(dispCondition, sizeof(dispCondition),	":dispdone%s\n", sSettingsNames[s]);
		autoexec += dispCondition;
	}
	
#ifdef SHIFT_VOLUME_MOD
	char volumeKeys[] = { 'V', 'M' };
#endif
	for( int s = NUM_TOGGLES; s < NUM_VOLUME; ++s )
	{
		char dispVar[32];
		char dispCondition[128];
		
		snprintf(dispVar, sizeof(dispVar), "%%%s%%", sSettingsNames[s]);
		snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], dispVar );
		WriteMenuLine(autoexec, dispCondition, strlen(dispVar)-1);
		
#ifdef SHIFT_VOLUME_MOD
		WriteMenuLine(autoexec, "");
		snprintf( dispCondition, sizeof(dispCondition), "            * Press         %c to lower.", volumeKeys[s-NUM_TOGGLES]);
		WriteMenuLine(autoexec, dispCondition);
		snprintf( dispCondition, sizeof(dispCondition), "            * Press Shift + %c to raise.", volumeKeys[s-NUM_TOGGLES]);
		WriteMenuLine(autoexec, dispCondition);
		WriteMenuLine(autoexec, "");
#endif
	}
	{
		int s = SOUNDTRACK_MODE;
		char dispCondition[128];

		snprintf(dispCondition, sizeof(dispCondition), "if \"%%%s%%\"==\"1\" goto dispon%s\n", sSettingsNames[s], sSettingsNames[s]);
		autoexec += dispCondition;
		snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], "LOOP ENTIRE SOUNDTRACK");
		WriteMenuLine(autoexec, dispCondition);
		snprintf(dispCondition, sizeof(dispCondition),	"goto dispdone%s\n"
														":dispon%s\n", sSettingsNames[s], sSettingsNames[s]);
		autoexec += dispCondition;

		snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], "LOOP SINGLE TRACK");
		WriteMenuLine(autoexec, dispCondition);

		snprintf(dispCondition, sizeof(dispCondition),	":dispdone%s\n", sSettingsNames[s]);
		autoexec += dispCondition;
	}
	{
		int s = SOUNDTRACK_BASE;
		char dispCondition[128];

		for( int t = sSettingsBase[s]; t < sSettingsBase[s]+NUM_TRACKS; ++t )
		{
			snprintf(dispCondition, sizeof(dispCondition), "if \"%%%s%%\"==\"%c\" goto disptrack_%c\n", sSettingsNames[s], t, t);
			autoexec += dispCondition;
		}
		for( int t = 0; t < NUM_TRACKS; ++t )
		{
			snprintf(dispCondition, sizeof(dispCondition), ":disptrack_%c\n", sSettingsBase[s]+t);
			autoexec += dispCondition;
			snprintf(dispCondition, sizeof(dispCondition), sSettingsDisp[s], sTrackNames[t]);
			WriteMenuLine(autoexec, dispCondition);
			autoexec += "goto disptrack_done\n";
		}
		autoexec += ":disptrack_done\n";
	}
	WriteMenuLine(autoexec, "");
	WriteMenuLine(autoexec, "    q - Return to main menu");
	WriteMenuLine(autoexec, "");
	autoexec += MENU_SEPARATOR
				"echo.\n"
				"choice "
#ifdef SHIFT_VOLUME_MOD
				"/s /c12VvMm56q"
#else
				"/c12gvkm56q"
#endif
				" Your choice\n"
				"if errorlevel 9 goto savesettings\n";

	autoexec += "if errorlevel 8 goto trackselection\n";
	autoexec += "if errorlevel 7 goto toggle";
	autoexec += sSettingsNames[SOUNDTRACK_MODE];
	autoexec += "\n";
	
	for( int s = NUM_VOLUME; s > NUM_TOGGLES; --s )
	{
		int down = NUM_TOGGLES + 2 * (s - NUM_TOGGLES);
		int up = down - 1;
		
		{
			char errorLevel[2] = { down + '0', '\0' };
			autoexec += "if errorlevel ";
			autoexec += errorLevel;
			autoexec += " goto down";
			autoexec += sSettingsNames[s-1];
			autoexec += "\n";
		}
		
		{
			char errorLevel[2] = { up + '0', '\0' };
			autoexec += "if errorlevel ";
			autoexec += errorLevel;
			autoexec += " goto up";
			autoexec += sSettingsNames[s-1];
			autoexec += "\n";
		}
	}
	for( int s = NUM_TOGGLES; s > 0; --s )
	{
		char errorLevel[2] = { s + '0', '\0' };
		autoexec += "if errorlevel ";
		autoexec += errorLevel;
		autoexec += " goto toggle";
		autoexec += sSettingsNames[s-1];
		autoexec += "\n";
	}

	autoexec += ":savesettings\n"
				"echo ";

	for( int s = 0; s < NUM_SET; ++s )
	{
		autoexec += "%";
		autoexec += sSettingsNames[s];
		autoexec += "%";
	}
	autoexec += ">" SETTINGS_FILENAME "\n"
				"goto mainmenu\n";

	GenBinaryToggle(autoexec, SOUNDTRACK_MODE);

	for( int i = 0; i < NUM_TOGGLES; ++i )
	{
		GenBinaryToggle(autoexec, i);
	}

	for( int i = NUM_TOGGLES; i < NUM_VOLUME; ++i )
	{
		autoexec += ":up";
		autoexec += sSettingsNames[i];
		autoexec += "\n";
		for( int v = 8; v >= 0; --v )
		{
			
			autoexec += "if \"%";
			autoexec += sSettingsNames[i];
			autoexec += "%\"==\"";
			
			char currV[2] = { v + '0', 0 };
			autoexec += currV;
			currV[0]++;
			autoexec += "\" set ";
			autoexec += sSettingsNames[i];
			autoexec += "=";
			autoexec += currV;
			autoexec += "\n";
		}
		autoexec += "goto settings\n";
		autoexec += ":down";
		autoexec += sSettingsNames[i];
		autoexec += "\n";
		for( int v = 1; v <= 9; ++v )
		{
			
			autoexec += "if \"%";
			autoexec += sSettingsNames[i];
			autoexec += "%\"==\"";
			
			char currV[2] = { v + '0', 0 };
			autoexec += currV;
			currV[0]--;
			autoexec += "\" set ";
			autoexec += sSettingsNames[i];
			autoexec += "=";
			autoexec += currV;
			autoexec += "\n";
		}
		autoexec += "goto settings\n";
	}

	autoexec += ":trackselection\n"
				"cls\n"
				MENU_SEPARATOR;
	WriteMenuLine(autoexec, "BASE TRACK SELECTION MENU");
	autoexec += MENU_SEPARATOR;

	//WriteMenuLine(autoexec, "");
	for( int t = 0; t < NUM_TRACKS; ++t )
	{
		char trackMenu[128];
		snprintf(trackMenu, sizeof(trackMenu), "    %c - %s", t + sSettingsBase[SOUNDTRACK_BASE], sTrackNames[t] );
		WriteMenuLine(autoexec, trackMenu);
	}
	WriteMenuLine(autoexec, "                                      Soundtrack composed by");
	WriteMenuLine(autoexec, "                                            Edwin Montgomery");
	//WriteMenuLine(autoexec, "");

	autoexec += MENU_SEPARATOR
				//"echo.\n"
				"choice /c";

	for( int t = 0; t < NUM_TRACKS; ++t )
	{
		char trackSel[2] = { sSettingsBase[SOUNDTRACK_BASE] + t, 0 };
		autoexec += trackSel;
	}

	autoexec += " Your choice?\n";
	for( int t = NUM_TRACKS; t > 0; --t )
	{
		char selection[64];
		snprintf(selection, sizeof(selection ), "if errorlevel %d goto selecttrack_%d\n", t, t);
		autoexec += selection;
	}
	for( int t = NUM_TRACKS; t > 0; --t )
	{
		char selection[64];
		snprintf(selection, sizeof(selection ), ":selecttrack_%d\nset %s=%c\n", t, sSettingsNames[SOUNDTRACK_BASE], sSettingsBase[SOUNDTRACK_BASE] + t - 1);
		autoexec += selection;
		autoexec += "goto settings\n";
	}
	
	if( hasContinueOption )
	{
		size_t saveCount = SetupGameSelectSubmenu(autoexec, saves, "selectgame", "LOAD GAME", "Select game you wish to load.", "loadsave");
		char action[128];
		for( size_t i = 0; i < saveCount; ++i )
		{
			snprintf(action, sizeof(action),	":loadsave%d\n"
												"set NEWGAME=%s\n"
												"goto launchgame\n", i+1, saves[i].c_str() );
			autoexec += action;
		}

		SetupGameSelectSubmenu(autoexec, saves, "wlreset",
												"UNOFFICIAL RESET UTILITY (WLRESET)",
												"Select game on which you wish to run utlity.",
												"savereset",
												UseWLResetAtOwnRisk);
		for( size_t i = 0; i < saveCount; ++i )
		{
			snprintf(action, sizeof(action),	":savereset%d\n"
												"set RESETGAME=%s\n"
												"goto resetgame\n", i+1, saves[i].c_str());
			autoexec += action;
		}
		autoexec += ":resetgame\n"
					"echo.\n"
					"echo.\n"
					"echo Please note!! Master Files were automatically setup when game was created.\n"
					"echo Recreating Master Files from utility will overwrite original maps!!\n"
					"echo.\n"
					"choice Would you like to view the WLRESET README before running utility?\n"
					"if errorlevel 2 goto runresetgame\n"
					"W:\\SHOW W:\\wlreset\\readme.txt\n"
					":runresetgame\n"
					"cd %RESETGAME%\n"
					"copy GAME1 GAME1.BU >NUL\n"
					"copy GAME2 GAME2.BU >NUL\n"
					"W:\\wlreset\\R.exe\n"
					"cd ..\n"
					"cls\n"
					"goto wlreset\n";

	}
	autoexec += ":launchgame\n"
				"echo %NEWGAME%>lastsave\n"
				"cd %NEWGAME%\n"
				"copy W:\\data\\WLA.BIN WLA.BIN >NUL\n"
				"W:\\WL.exe\n"
				":leavegame\n"
				"exit\n"
				":about\n"
				"W:\\SHOW X:\\EULA.txt\n"
				"goto mainmenu\n"
				":skiptoshell\n";

	sMountSave->Install(autoexec);

	FILE* f;

	/*
	f = fopen("autoexec.txt", "w");
	fwrite(autoexec.c_str(), 1, strlen(autoexec.c_str()), f);
	fclose(f);
	*/

	f = fopen("portraits.bin", "rb");
	fseek( f, 0, SEEK_END );
	size_t dataSize = ftell(f);
	spPortraits = (Portraits*)malloc( dataSize );
	fseek( f, 0, SEEK_SET );
	fread( spPortraits, 1, dataSize, f );
	fclose( f );

	new (spPortraits) Portraits();

	spPortraits->LoadOverrides();

	f = fopen("cursors.bin", "rb");
	spCursors = new Cursors;
	fread( spCursors, 1, sizeof(Cursors), f );
	fclose( f );

	spParagraphs = new Paragraphs;
	spLegal = new Legal;
}

void WastelandEXT::Purge()
{
	delete [] sPixels;
	delete spCursors;
	delete spParagraphs;
	delete spLegal;

	delete sMountSave;

	OGG_safedestroy(voiceover);
	OGG_safedestroy(music);
}

extern void RENDER_ForceNormal3x();
#ifdef MACOSX
extern void RENDER_ForceAdvMame3x();
#endif

static int sCurrTrack;
static void PlayMusic()
{
	float musicScale = float(sSettings[MUSIC]) / 9;
	char track[16];
	snprintf(track, sizeof(track), "music/%02d.ogg", sCurrTrack);
	music = OGG_new(track);
	OGG_play(music, sSettings[SOUNDTRACK_MODE] );
	OGG_setvolume(music, int(musicScale*SDL_MIX_MAXVOLUME));
}

bool WastelandEXT::PreUpdate(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bitu outPitch)
{
	if( (width == 320) && (height == 200) && (bpp == 8) && (pitch == 320) ) //&& (outPitch == 3840) )
	{
		if( !sSettingsLoaded )
		{
			std::string path;
			Cross::GetPlatformConfigDir(path);
			LoadSettings(path + "/" SETTINGS_FILENAME );

			if( !sSettings[SMOOTHING] )
			{
				RENDER_ForceNormal3x();
			}
#ifdef MACOSX
			else
			{
				RENDER_ForceAdvMame3x();
			}
#endif
			if( sSettings[MUSIC] )
			{
				sCurrTrack = sSettings[SOUNDTRACK_BASE];

				PlayMusic();
			}
			sSettingsLoaded = true;
		}

		if( sSettings[MUSIC] && music && !OGG_playing(music) )
		{
			sCurrTrack = ( sCurrTrack + 1 ) % NUM_TRACKS;
			PlayMusic();
		}
		rectCount = 0;
		sPixelCache = GFX_GetBlitPix(TARGET_W, TARGET_H);
		if( TARGET_W*TARGET_H > MAX_TARGET_W*MAX_TARGET_H )
		{
			if( sPixels )
			{
				delete [] sPixels;
			}
			MAX_TARGET_W = TARGET_W; MAX_TARGET_H = TARGET_H;
			sPixels = new Bit8u[ MAX_TARGET_W * MAX_TARGET_H * 4 ];
		}

		if( sPixelCache )
		{
			spLegal->Update(width, height, bpp, pitch, data, pal, (Bit8u*)sPixelCache, outPitch);

			if( sSettings[PORTRAITS] )
			{
				spPortraits->Update(width, height, bpp, pitch, data, pal, (Bit8u*)sPixelCache, outPitch);
			}
			spParagraphs->Update(width, height, bpp, pitch, data, pal, (Bit8u*)sPixelCache, outPitch);
		}
	}
	return ( rectCount > 0 || rectCountPrev > 0 );
}

void WastelandEXT::Update( SDL_Surface* surface )
{
	if( rectCount > 0 || rectCountPrev > 0 )
	{
		int rectCountCurr = rectCount;
		for( int prev = 0; prev < rectCountPrev; ++prev )
		{
			SDL_Rect* pPrev = &updateRectsPrev[prev];
			bool dirty = true;
			for( int curr = 0; curr < rectCount; ++curr )
			{
				SDL_Rect* pCurr = &updateRects[curr];
				
				if( pPrev->x == pCurr->x &&
					pPrev->y == pCurr->y &&
					pPrev->w == pCurr->w &&
					pPrev->h == pCurr->h )
				{
					dirty = false;
					break;
				}
			}

			if( dirty )
			{
				updateRects[rectCountCurr++] = *pPrev;
			}
		}
		SDL_UpdateRects( surface, rectCountCurr, updateRects );
	}

	rectCountPrev = rectCount;
	memcpy(updateRectsPrev, updateRects, sizeof(updateRectsPrev));
}

struct Input
{
	Input() : locked(false) {}

	enum Event
	{
		ESC,
		UP,
		DOWN,
		MOUSE,
		PGUP,
		PGDOWN,
		COUNT
	};

	void Update()
	{
		prev = curr;
		curr = postUpdate;
		memset(&pendingUpdate, 0, sizeof(pendingUpdate));
	}

	struct State
	{
		bool events[COUNT];
	}
	prev, curr, pendingUpdate, postUpdate;

	void SetEvent(Event e, bool val)
	{
		if( !pendingUpdate.events[e] )
		{
			curr.events[e] = val;
			pendingUpdate.events[e] = true;
		}
		postUpdate.events[e] = val;
	}

	bool IsTriggered(Event e)
	{
		return !prev.events[e] && curr.events[e];
	}

	bool IsReleased(Event e)
	{
		return prev.events[e] && !curr.events[e];
	}

	bool locked;
};

static Input sInput;

void WastelandEXT::PostUpdate()
{
	if( rectCount > 0 )
	{
		memcpy( sPixelCache, sPixels, TARGET_W*TARGET_H*4 );
	}
	sInput.Update();
}

bool WastelandEXT::KeyEvent(KBD_KEYS keytype,bool pressed)
{
	switch( keytype )
	{
		case KBD_esc:
			sInput.SetEvent( Input::ESC, pressed );
			break;

		case KBD_up:
			sInput.SetEvent( Input::UP, pressed );
			break;

		case KBD_down:
			sInput.SetEvent( Input::DOWN, pressed );
			break;

		case KBD_pagedown:
			sInput.SetEvent( Input::PGDOWN, pressed );
			break;

		case KBD_pageup:
			sInput.SetEvent( Input::PGUP, pressed );
			break;
	}
	return sInput.locked;
}

bool WastelandEXT::MouseEvent(int button, bool pressed)
{
	switch( button )
	{
		case 0:
			sInput.SetEvent(Input::MOUSE, pressed);
			break;

		case 3:
			sInput.SetEvent(Input::UP, pressed);
			break;
		case 4:
			sInput.SetEvent(Input::DOWN, pressed);
			break;
	}
	return sInput.locked;
}

static const int LINE_H = 8;
static const int NUM_LINES = 6;
static int sParagraphDisplay = 0, sParagraphOffset, sParagraphTargetOffset;
void Paragraphs::Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch)
{
	if( sSettings[MUSIC] )
	{
		float musicScale = float(sSettings[MUSIC]) / 9;
		if( voiceover && OGG_playing(voiceover) )
		{
			OGG_setvolume( music, max( music->volume-2, int(musicScale*(SDL_MIX_MAXVOLUME/3))) );
		}
		else
		{
			OGG_setvolume( music, min( music->volume+2, int(musicScale*(SDL_MIX_MAXVOLUME))) );
		}
	}

	if( sParagraphDisplay )
	{
		int totalH = frame[TOP]->h + frame[LEFT]->h + frame[BOTTOM]->h;
		int frameX = ( 960 - frame[TOP]->w ) / 2;
		int frameY = ( 600 - totalH ) / 2;

		int textY = frameY + frame[TOP]->h;
		int textX = frameX + frame[LEFT]->w;

		frame[TOP]->Blit( frameX, frameY, outWrite, outPitch );
		frame[LEFT]->Blit( frameX, textY, outWrite, outPitch );
		int frameRightX = frameX + frame[TOP]->w - frame[RIGHT]->w;
		frame[RIGHT]->Blit( frameRightX, textY, outWrite, outPitch );
		int mx, my;
		Mouse_GetHQ3XCursor(mx, my);
		int frameBottomY = textY + frame[LEFT]->h;

		bool cursorInESC = ( mx >= (frameX+314) && mx <= (frameX+314+348) ) &&
						   ( my >= frameBottomY && my <= frameBottomY + frame[BOTTOM]->h );
		frame[cursorInESC ? BOTTOM_HI : BOTTOM]->Blit( frameX, frameBottomY, outWrite, outPitch );
		int cursor = cursorInESC ? 1 : 0;

		int maxParagraphOffset = paragraphs[sParagraphDisplay]->h - frame[LEFT]->h;
		const int lineH = 19;
		const int pageH = frame[LEFT]->h - lineH;

		if( maxParagraphOffset > 0 )
		{

			int buttonX = frameRightX + frame[RIGHT]->w - buttons[DOWN]->w;
			int buttonUpY = textY + 20;
			int buttonDownY = textY + frame[LEFT]->h - 20 - buttons[DOWN]->h;

			bool cursorInUp =	( mx >= buttonX && mx <= buttonX + buttons[UP]->w ) &&
								( my >= buttonUpY && my <= buttonUpY + buttons[UP]->h );

			bool cursorInDown =	( mx >= buttonX && mx <= buttonX + buttons[DOWN]->w ) &&
								( my >= buttonDownY && my <= buttonDownY + buttons[DOWN]->h );

			buttons[cursorInUp ? UP_HI : UP]->Blit( buttonX, buttonUpY, outWrite, outPitch );
			buttons[cursorInDown ? DOWN_HI : DOWN]->Blit( buttonX, buttonDownY, outWrite, outPitch );

			if( cursorInUp || cursorInDown )
			{
				cursor = 1;
			}
			if( sInput.IsTriggered(Input::UP) || ( cursorInUp && sInput.IsTriggered(Input::MOUSE) ) )
			{
				sParagraphTargetOffset = sParagraphOffset - lineH;
				if( sParagraphTargetOffset < 0 ) { sParagraphTargetOffset = 0; }
			}

			if( sInput.IsTriggered(Input::DOWN) || ( cursorInDown && sInput.IsTriggered(Input::MOUSE) ) )
			{
				sParagraphTargetOffset = sParagraphOffset + lineH;
				if( sParagraphTargetOffset > maxParagraphOffset ) { sParagraphTargetOffset = maxParagraphOffset; }
			}
			if( sInput.IsTriggered(Input::PGUP) )
			{
				sParagraphTargetOffset = sParagraphOffset - pageH;
				if( sParagraphTargetOffset < 0 ) { sParagraphTargetOffset = 0; }
			}

			if( sInput.IsTriggered(Input::PGDOWN) )
			{
				sParagraphTargetOffset = sParagraphOffset + pageH;
				if( sParagraphTargetOffset > maxParagraphOffset ) { sParagraphTargetOffset = maxParagraphOffset; }
			}

		}
		if( sParagraphOffset < sParagraphTargetOffset )
		{
			sParagraphOffset = sParagraphTargetOffset;
			//sParagraphOffset++;
		}
		else if( sParagraphOffset > sParagraphTargetOffset )
		{
			sParagraphOffset = sParagraphTargetOffset;
			//sParagraphOffset--;
		}
		paragraphs[sParagraphDisplay]->BlitClipped(textX, textY, sParagraphOffset, frame[LEFT]->h, outWrite, outPitch);
		if( sInput.IsTriggered(Input::ESC) || ( cursorInESC && sInput.IsTriggered(Input::MOUSE) ) )
		{
			sParagraphDisplay = 0;
			sInput.locked = false;

			if( sSettings[VOICE] )
			{
				OGG_safedestroy(voiceover);
			}
		}
		spCursors->CheckForOverlap(width, height, bpp, pitch, data, pal, outWrite, outPitch, frameX/3, frameY/3, frame[TOP]->w/3+1, totalH/3+1, cursor);
		return;
	}

	int manualNeeded = -1;
	for( int m = 0; m < MANUAL; ++m )
	{
		Bit8u* exactCompPos = data + 8 * pitch + 112;
		if( charRef[m]->Compare( exactCompPos, pal, pitch, Cursors::GetMaxOverlapArea(charRef[m]) ) )
		{
			manualNeeded = m;
			break;
		}
	}

	int mx, my;
	Mouse_GetHQ3XCursor(mx, my);

	if( manualNeeded >= 0 )
	{
		int qX = 888;
		if( mx >= qX && mx <= qX + question[0]->w && my >= 0 && my <= question[0]->h )
		{
			if( sInput.IsTriggered(Input::MOUSE) )
			{
				sParagraphOffset = 0;
				sParagraphTargetOffset = -1;
				sParagraphDisplay = PARAGRAPH + manualNeeded;
				sInput.locked = true;
			}
			question[1]->Blit( qX, 0, outWrite, outPitch );
		}
		else
		{
			question[0]->Blit( qX, 0, outWrite, outPitch );
		}
		spCursors->CheckForOverlap(width, height, bpp, pitch, data, pal, outWrite, outPitch, qX/3, 0, (question[0]->w)/3+1, (question[0]->h)/3+1);
	}

	//scan the story box.
	Bit8u* lineP = data + SCAN_Y * pitch + SCAN_X;
	for( int y = SCAN_Y, line = 0; line < NUM_LINES; y += LINE_H, line++ )
	{
		Bit8u* row = lineP;
		for( int x = SCAN_X; x < SCR_W - ref->w; x+=8 )
		{
			if( ref->CompareText( row, pal, pitch ) )
			{
				DisplayBook(x, y, mx, my, lineP, width, height, bpp, pitch, data, pal, outWrite, outPitch, SCAN_X);
			}
			row += 8;
		}
		lineP += LINE_H * pitch;
	}

	//scan the encounter box
	const int ENCOUNTER_BOX_X = 120, ENCOUNTER_BOX_Y = 8;
	const int ENCOUNTER_H = 96;
	lineP = data + ENCOUNTER_BOX_Y * pitch + ENCOUNTER_BOX_X;
	for( int y = ENCOUNTER_BOX_Y; y < ENCOUNTER_BOX_Y+ENCOUNTER_H; y ++ )
	{
		Bit8u* row = lineP;
		for( int x = ENCOUNTER_BOX_X; x < SCR_W - ref->w; x+=8 )
		{
			if( ref->CompareText( row, pal, pitch ) )
			{
				DisplayBook(x, y, mx, my, lineP, width, height, bpp, pitch, data, pal, outWrite, outPitch, ENCOUNTER_BOX_X);
			}
			row += 8;
		}
		lineP += pitch;
	}
}

void Paragraphs::DisplayBook(int paragraphX, int paragraphY, int mx, int my, Bit8u* lineStart, Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch, int startLineOffset)
{
	int paragraphNumber = 0;
	int lastNumberX = 0;
	for( int x = paragraphX + ref->w; x < SCR_W - numbers[0]->w; ++x )
	{
		Bit8u* lineP = lineStart + x;
		for( int n = 0; n < NUMBER; ++n )
		{
			if( numbers[n]->Compare( lineP, pal, pitch ) )
			{
				lastNumberX = x;
				paragraphNumber *= 10;
				paragraphNumber += n;
				lineP += numbers[n]->w;
				break;
			}
		}
		lineP++;
	}

	//end of line?
	if( paragraphNumber == 0 )
	{
		Bit8u* lineP = lineStart + pitch * LINE_H;

		for( int x = startLineOffset; x < SCR_W - numbers[0]->w; ++x )
		{
			for( int n = 0; n < NUMBER; ++n )
			{
				if( numbers[n]->Compare( lineP, pal, pitch ) )
				{
					lastNumberX = x;
					paragraphNumber *= 10;
					paragraphNumber += n;
					lineP += numbers[n]->w;
					break;
				}
			}
			lineP++;
		}
		
	}
	int bookX = 3 * paragraphX;
	int bookY = 3 * paragraphY;
	int bookW = ref->w * 3;
	int bookH = ref->h * 3;
	if( mx >= bookX && mx <= bookX + bookW && my >= bookY && my <= bookY + bookH )
	{
		if( sInput.IsTriggered(Input::MOUSE) && paragraphNumber )
		{
			sParagraphOffset = 0;
			sParagraphTargetOffset = -1;
			sParagraphDisplay = paragraphNumber;
			sInput.locked = true;

			if( sSettings[VOICE] )
			{
				float voiceScale = float(sSettings[VOICE]) / 9;
				char snd[16];
				snprintf(snd, sizeof(snd), "vo/%03d.ogg", paragraphNumber);
				voiceover = OGG_new(snd);
				OGG_play(voiceover, 0);
				OGG_setvolume(voiceover, int(voiceScale*SDL_MIX_MAXVOLUME) );
			}
		}
		MultiplyAndUnderline( outWrite, outPitch, bookX, bookY, bookW, bookH, 255, 255, 85 );
	}
	else
	{
		MultiplyAndUnderline( outWrite, outPitch, bookX, bookY, bookW, bookH, 255, 85, 85 );
	}
	spCursors->CheckForOverlap(width, height, bpp, pitch, data, pal, outWrite, outPitch, bookX/3, bookY/3, bookW/3+1, bookH/3+1);
	
}

void Portraits::Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch)
{
	float maxMatchPerc = 0.0f;
	int maxMatchIdx = -1;
	LD src( width, height, bpp, pitch, data,  pal );

	Bit8u* exactFrameRightPos = data + 0 * pitch + 104;
	Bit8u* exactFrameBottomPos = data + 104 * pitch + 0;
	if( src.IsNotBlack() &&
		frameRight->Compare( exactFrameRightPos, pal, pitch, Cursors::GetMaxOverlapArea(frameRight) ) &&
		frameBottom->Compare( exactFrameBottomPos, pal, pitch, Cursors::GetMaxOverlapArea(frameBottom) ) )
	{
		for( int i = 0; i < COUNT; ++i )
		{
			float matchPerc = src.Compare( map[i].ld );
			if( matchPerc > maxMatchPerc )
			{
				maxMatchPerc = matchPerc;
				maxMatchIdx = i;
			}
		}

		if( maxMatchIdx >= 0 /*&& maxMatchPerc >= 0.75f*/ )
		{
			static bool dump = false;
			if( dump )
			{
				map[maxMatchIdx].ld.Dump( "match.tga" );
				src.Dump( "screen.tga" );
				dump = false;
			}
			map[maxMatchIdx].hd.Blit( outWrite, outPitch );

			spCursors->CheckForOverlap(width, height, bpp, pitch, data, pal, outWrite, outPitch, LD::X, LD::Y, LD::W, LD::H);
		}
	}
}

void Legal::Update(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal, Bit8u *outWrite, Bitu outPitch)
{
	bool skipTopClip = false;
	for( int i = 0; i < COUNT; ++i )
	{
		Bit8u* exactCompPos = data + Paragraphs::SCAN_Y * pitch + REF_X;
		GrayImg* pRef = ref[i];
		int numLinesOut = (pRef->h / LINE_H) - 1;
		bool found = false;
		GrayImg* pBlit = sSettings[SMOOTHING] ? hq[i] : nrm[i];
		
		for( int l = 0; !found && !skipTopClip && l < numLinesOut; ++l )
		{
			int cmpYOffset = (numLinesOut - l) * LINE_H;
			int cmpMaxH = pRef->h - cmpYOffset;
			if( pRef->CompareClipped( exactCompPos, pal, pitch, cmpYOffset, cmpMaxH, Cursors::GetMaxOverlapArea(pRef) ) )
			{
				int yOffset = cmpYOffset * 3;
				int blitH = pBlit->h - yOffset;
				pBlit->BlitClipped(BLIT_X, BLIT_Y, yOffset, blitH, outWrite, outPitch);
				spCursors->CheckForOverlap(pBlit->w, blitH, bpp, pitch, data, pal, outWrite, outPitch, BLIT_X/3, BLIT_Y/3, pBlit->w/3+1, blitH/3+1, 0);
				found = true;
				skipTopClip = true;
			}
		}

		for( int l = 0; !found && l < NUM_LINES - numLinesOut; ++l )
		{
			if( pRef->Compare( exactCompPos, pal, pitch, Cursors::GetMaxOverlapArea(pRef) ) )
			{
				int blitY = BLIT_Y + l * LINE_H * 3; 
				pBlit->BlitClipped(BLIT_X, blitY, 0, pBlit->h, outWrite, outPitch);
				spCursors->CheckForOverlap(pBlit->w, pBlit->h, bpp, pitch, data, pal, outWrite, outPitch, BLIT_X/3, blitY/3, pBlit->w/3+1, pBlit->h/3+1, 0);
				found = true;
			}
			exactCompPos += pitch * LINE_H;
		}

		for( int l = 0; !found && l < numLinesOut; ++l )
		{
			int cmpMaxH = pRef->h - (l + 1) * LINE_H;
			if( pRef->CompareClipped( exactCompPos, pal, pitch, 0, cmpMaxH, Cursors::GetMaxOverlapArea(pRef) ) )
			{
				int blitY = BLIT_Y + (NUM_LINES - (numLinesOut - l)) * LINE_H * 3;
				int blitH = pBlit->h - (l + 1) * LINE_H * 3;
				pBlit->BlitClipped(BLIT_X, blitY, 0, blitH, outWrite, outPitch);
				spCursors->CheckForOverlap(pBlit->w, blitH, bpp, pitch, data, pal, outWrite, outPitch, BLIT_X/3, blitY/3, pBlit->w/3+1, blitH/3+1, 0);
				found = true;
			}
			exactCompPos += pitch * LINE_H;
		}
	}
}
//grab the portrait screen area
Portraits::LD::LD(Bitu width, Bitu height, Bitu bpp, Bitu pitch, Bit8u * data, Bit8u * pal)
{
	assert(bpp == 8 && pal);
	Bit8u* src = data + X + Y * pitch;
	Pix* dst = pix;
	for( int y = 0; y < H; ++y )
	{
		Bit8u* row = src + y * pitch;
		for( int x = 0; x < W; ++x )
		{
			Bit8u idx = row[x];
			Bit8u* srcPix = pal + idx * 4;

			dst->r = srcPix[0];
			dst->g = srcPix[1];
			dst->b = srcPix[2];

			++dst;
		}
	}
}

void Portraits::LD::Dump( const char* filename )
{
	FILE* f = fopen(filename, "wb");

	typedef unsigned char u8;
	typedef signed short s16;

	struct TGA_HEADER
	{
		u8 identsize;          // size of ID field that follows 18 byte header (0 usually)
		u8 colourmaptype;      // type of colour map 0=none, 1=has palette
		u8 imagetype;          // type of image 0=none,1=indexed,2=rgb,3=grey,+8=rle packed

		u8 colourmapstart[2];     // first colour map entry in palette
		u8 colourmaplength[2];    // number of colours in palette
		u8 colourmapbits;      // number of bits per palette entry 15,16,24,32

		s16 xstart;             // image x origin
		s16 ystart;             // image y origin
		s16 width;              // image width in pixels
		s16 height;             // image height in pixels
		u8 bits;               // image bits per pixel 8,16,24,32
		u8 descriptor;         // image descriptor bits (vh flip bits)

		// pixel data follows header

	};
	TGA_HEADER header = { 0, 0, 2, {0}, {0}, 0, 0, 0, W, H, 24, 0x28 };
	fwrite( &header, 1, sizeof(header), f );
	fwrite( pix, 1, sizeof(pix), f );

	fclose( f );
}

float Portraits::LD::Compare( const LD& other ) const
{
	int countEq = 0;
	const Pix* thisPix = pix;
	const Pix* otherPix = other.pix;

	for( int i = 0; i < W*H; ++i )
	{
		if( thisPix->r == otherPix->r &&
			thisPix->g == otherPix->g &&
			thisPix->b == otherPix->b )
		{
			countEq++;
		}
		++thisPix;
		++otherPix;
	}
	return float(countEq) / (W * H);
}

void Portraits::HD::Blit( Bit8u* outWrite, Bitu outPitch )
{
	BeginBlit(X, Y, W, H);

	Bit8u* dst = outWrite + (Y + CLIP_Y) * outPitch + (X + CLIP_X) * 4;
	Pix* src = pix;

	for( int y = 0; y < H; ++y )
	{
		Bit8u* row = dst + y * outPitch;
		for( int x = 0; x < W; ++x )
		{
#ifdef MACOSX
			row[3] = src->b;
			row[2] = src->g;
			row[1] = src->r;
#else
			row[0] = src->b;
			row[1] = src->g;
			row[2] = src->r;
#endif
			row += 4;
			++src;
		}
	}
}
