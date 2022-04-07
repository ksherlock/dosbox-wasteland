/* This file supports Ogg Vorbis stream streams */

#include "SDL.h"
#include "stream_ogg.h"

/* This is the format of the audio mixer data */
static SDL_AudioSpec mixer;

/* Initialize the Ogg Vorbis player, with the given mixer settings
   This function returns 0, or -1 if there was an error.
 */
int OGG_init(SDL_AudioSpec *mixerfmt)
{
	mixer = *mixerfmt;
	return(0);
}

/* Set the volume for an OGG stream */
void OGG_setvolume(OGG_stream *stream, int volume)
{
	stream->volume = volume;
}

/* Load an OGG stream from the given file */
OGG_stream *OGG_new(const char *file)
{
	OGG_stream *stream;
	FILE *fp;

	stream = (OGG_stream *)malloc(sizeof *stream);
	if ( stream ) {
		/* Initialize the stream structure */
		memset(stream, 0, (sizeof *stream));
		OGG_stop(stream);
		OGG_setvolume(stream, SDL_MIX_MAXVOLUME);
		stream->section = -1;

		fp = fopen(file, "rb");
		if ( fp == NULL ) {
			SDL_SetError("Couldn't open %s", file);
			free(stream);
			return(NULL);
		}
		if ( ov_open(fp, &stream->vf, NULL, 0) < 0 ) {
			SDL_SetError("Not an Ogg Vorbis audio stream");
			free(stream);
			fclose(fp);
			return(NULL);
		}
	} else {
		SDL_OutOfMemory();
	}
	return(stream);
}

/* Start playback of a given OGG stream */
void OGG_play(OGG_stream *stream, int loop)
{
	stream->playing = 1;
	stream->loop = loop;
}

/* Return non-zero if a stream is currently playing */
int OGG_playing(OGG_stream *stream)
{
	return(stream->playing);
}

/* Read some Ogg stream data and convert it for output */
static void OGG_getsome(OGG_stream *stream)
{
	int section;
	int len;
	char data[4096];
	SDL_AudioCVT *cvt;

	len = ov_read(&stream->vf, data, sizeof(data), 0, 2, 1, &section);
	if ( len <= 0 ) {
		if ( len == 0 ) {
			if( stream->loop ) {
				ov_pcm_seek(&stream->vf, 0);
				len = ov_read(&stream->vf, data, sizeof(data), 0, 2, 1, &section);
			}
			else
				stream->playing = 0;
		}
		return;
	}
	cvt = &stream->cvt;
	if ( section != stream->section ) {
		vorbis_info *vi;

		vi = ov_info(&stream->vf, -1);
		SDL_BuildAudioCVT(cvt, AUDIO_S16, vi->channels, vi->rate,
		                       mixer.format,mixer.channels,mixer.freq);
		if ( cvt->buf ) {
			free(cvt->buf);
		}
		cvt->buf = (Uint8 *)malloc(sizeof(data)*cvt->len_mult);
		stream->section = section;
	}
	if ( cvt->buf ) {
		memcpy(cvt->buf, data, len);
		if ( cvt->needed ) {
			cvt->len = len;
			SDL_ConvertAudio(cvt);
		} else {
			cvt->len_cvt = len;
		}
		stream->len_available = stream->cvt.len_cvt;
		stream->snd_available = stream->cvt.buf;
	} else {
		SDL_OutOfMemory();
		stream->playing = 0;
	}
}

/* Play some of a stream previously started with OGG_play() */
void OGG_playAudio(OGG_stream *stream, Uint8 *snd, int len)
{
	int mixable;

	while ( (len > 0) && stream->playing ) {
		if ( ! stream->len_available ) {
			OGG_getsome(stream);
		}
		mixable = len;
		if ( mixable > stream->len_available ) {
			mixable = stream->len_available;
		}
		/*
		if ( stream->volume == SDL_MIX_MAXVOLUME ) {
			memcpy(snd, stream->snd_available, mixable);
		} else */ {
			SDL_MixAudio(snd, stream->snd_available, mixable,
			                              stream->volume);
		}
		stream->len_available -= mixable;
		stream->snd_available += mixable;
		len -= mixable;
		snd += mixable;
	}
}

/* Stop playback of a stream previously started with OGG_play() */
void OGG_stop(OGG_stream *stream)
{
	stream->playing = 0;
}

/* Close the given OGG stream */
void OGG_delete(OGG_stream *stream)
{
	if ( stream ) {
		if ( stream->cvt.buf ) {
			free(stream->cvt.buf);
		}
		ov_clear(&stream->vf);
		free(stream);
	}
}

// fill in allocator
#undef _ogg_malloc
#undef _ogg_calloc
#undef _ogg_realloc
#undef _ogg_free

void* _ogg_malloc( size_t size )
{
	return malloc( size );
}

void* _ogg_calloc( size_t nelem, size_t elsize )
{
	return calloc( nelem, elsize );
}

void* _ogg_realloc( void* ptr, size_t size )
{
	return realloc( ptr, size );
}

void  _ogg_free( void* ptr )
{
	free( ptr );
}

