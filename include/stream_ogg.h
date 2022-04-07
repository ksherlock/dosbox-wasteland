#ifndef __OGG_STREAM__
#define __OGG_STREAM__

/* This file supports Ogg Vorbis music streams */

#include "vorbis/vorbisfile.h"

typedef struct {
	int playing, loop;
	int volume;
	OggVorbis_File vf;
	int section;
	SDL_AudioCVT cvt;
	int len_available;
	Uint8 *snd_available;
} OGG_stream;

#ifdef __cplusplus
extern "C" {
#endif
	
/* Initialize the Ogg Vorbis player, with the given mixer settings
   This function returns 0, or -1 if there was an error.
 */
int OGG_init(SDL_AudioSpec *mixer);

/* Set the volume for an OGG stream */
void OGG_setvolume(OGG_stream *stream, int volume);

/* Load an OGG stream from the given file */
OGG_stream *OGG_new(const char *file);

/* Start playback of a given OGG stream */
void OGG_play(OGG_stream *stream, int loop);

/* Return non-zero if a stream is currently playing */
int OGG_playing(OGG_stream *stream);

/* Play some of a stream previously started with OGG_play() */
void OGG_playAudio(OGG_stream *stream, Uint8 *snd, int len);

/* Stop playback of a stream previously started with OGG_play() */
void OGG_stop(OGG_stream *stream);

/* Close the given OGG stream */
void OGG_delete(OGG_stream *stream);

#ifdef __cplusplus
}
#endif

#endif /* OGG_STREAM */
