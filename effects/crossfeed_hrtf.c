#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "crossfeed_hrtf.h"
#include "../codec.h"
#include "../util.h"

struct crossfeed_hrtf_state {
	fftw_complex *filter_fr_left[2];
	fftw_complex *filter_fr_right[2];
	fftw_complex *tmp_fr[2];
	sample_t *input[2];
	sample_t *output_left_input[2];
	sample_t *output_right_input[2];
	sample_t *overlap_left_input[2];
	sample_t *overlap_right_input[2];
	fftw_plan plan_r2c_left_c0, plan_r2c_left_c1, plan_c2r_left_c0, plan_c2r_left_c1;
	fftw_plan plan_r2c_right_c0, plan_r2c_right_c1, plan_c2r_right_c0, plan_c2r_right_c1;
	ssize_t input_frames, fr_frames, buf_pos, drain_pos;
	int has_output;
};

void crossfeed_hrtf_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	ssize_t i, iframes = 0, oframes = 0;

	while (iframes < *frames) {
		while (state->buf_pos < state->input_frames / 2 && iframes < *frames) {
			state->input[0][state->buf_pos] = ibuf[iframes * 2 + 0];
			state->input[1][state->buf_pos] = ibuf[iframes * 2 + 1];
			++iframes;

			if (state->has_output) {
				/* sum left ear output */
				obuf[oframes * 2 + 0] = state->output_left_input[0][state->buf_pos] + state->output_right_input[0][state->buf_pos];
				/* sum right ear output */
				obuf[oframes * 2 + 1] = state->output_left_input[1][state->buf_pos] + state->output_right_input[1][state->buf_pos];
				++oframes;
			}

			++state->buf_pos;
		}

		if (state->buf_pos == state->input_frames / 2) {
			/* left input */
			fftw_execute(state->plan_r2c_left_c0);
			fftw_execute(state->plan_r2c_left_c1);
			for (i = 0; i < state->fr_frames; ++i) {
				/* channel 0 */
				state->tmp_fr[0][i] *= state->filter_fr_left[0][i];
				/* channel 1 */
				state->tmp_fr[1][i] *= state->filter_fr_left[1][i];
			}
			fftw_execute(state->plan_c2r_left_c0);
			fftw_execute(state->plan_c2r_left_c1);
			/* normalize */
			for (i = 0; i < state->input_frames; ++i) {
				state->output_left_input[0][i] /= state->input_frames;
				state->output_left_input[1][i] /= state->input_frames;
			}
			/* handle overlap */
			for (i = 0; i < state->input_frames / 2; ++i) {
				state->output_left_input[0][i] += state->overlap_left_input[0][i];
				state->output_left_input[1][i] += state->overlap_left_input[1][i];
				state->overlap_left_input[0][i] = state->output_left_input[0][i + state->input_frames / 2];
				state->overlap_left_input[1][i] = state->output_left_input[1][i + state->input_frames / 2];
			}

			/* right input */
			fftw_execute(state->plan_r2c_right_c0);
			fftw_execute(state->plan_r2c_right_c1);
			for (i = 0; i < state->fr_frames; ++i) {
				/* channel 0 */
				state->tmp_fr[0][i] *= state->filter_fr_right[0][i];
				/* channel 1 */
				state->tmp_fr[1][i] *= state->filter_fr_right[1][i];
			}
			fftw_execute(state->plan_c2r_right_c0);
			fftw_execute(state->plan_c2r_right_c1);
			/* normalize */
			for (i = 0; i < state->input_frames; ++i) {
				state->output_right_input[0][i] /= state->input_frames;
				state->output_right_input[1][i] /= state->input_frames;
			}
			/* handle overlap */
			for (i = 0; i < state->input_frames / 2; ++i) {
				state->output_right_input[0][i] += state->overlap_right_input[0][i];
				state->output_right_input[1][i] += state->overlap_right_input[1][i];
				state->overlap_right_input[0][i] = state->output_right_input[0][i + state->input_frames / 2];
				state->overlap_right_input[1][i] = state->output_right_input[1][i + state->input_frames / 2];
			}
			state->buf_pos = 0;
			state->has_output = 1;
		}
	}
	*frames = oframes;
}

void crossfeed_hrtf_effect_reset(struct effect *e)
{
	ssize_t i;
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	state->buf_pos = 0;
	state->has_output = 0;
	for (i = 0; i < state->input_frames / 2; ++i) {
		state->overlap_left_input[0][i] = 0;
		state->overlap_left_input[1][i] = 0;
		state->overlap_right_input[0][i] = 0;
		state->overlap_right_input[1][i] = 0;
	}
}

void crossfeed_hrtf_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	sample_t *ibuf;
	ssize_t npad = state->input_frames / 2 - state->buf_pos, oframes = 0;
	npad = (npad > *frames) ? *frames : npad;
	if (state->has_output && state->buf_pos != 0 && npad > 0) {
		ibuf = calloc(npad * e->ostream.channels, sizeof(sample_t));
		crossfeed_hrtf_effect_run(e, &npad, ibuf, obuf);
		free(ibuf);
		*frames = npad;
	}
	else if (state->has_output) {
		while (state->drain_pos < state->input_frames / 2 && oframes < *frames) {
			/* sum left ear output */
			obuf[oframes * 2 + 0] = state->output_left_input[0][state->drain_pos] + state->output_right_input[0][state->drain_pos];
			/* sum right ear output */
			obuf[oframes * 2 + 1] = state->output_left_input[1][state->drain_pos] + state->output_right_input[1][state->drain_pos];
			++oframes;
			++state->drain_pos;
		}
		*frames = oframes;
	}
	else
		*frames = 0;
}

void crossfeed_hrtf_effect_destroy(struct effect *e)
{
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	fftw_free(state->filter_fr_left[0]);
	fftw_free(state->filter_fr_left[1]);
	fftw_free(state->filter_fr_right[0]);
	fftw_free(state->filter_fr_right[1]);
	fftw_free(state->tmp_fr[0]);
	fftw_free(state->tmp_fr[1]);
	free(state->input[0]);
	free(state->input[1]);
	free(state->output_left_input[0]);
	free(state->output_left_input[1]);
	free(state->output_right_input[0]);
	free(state->output_right_input[1]);
	free(state->overlap_left_input[0]);
	free(state->overlap_left_input[1]);
	free(state->overlap_right_input[0]);
	free(state->overlap_right_input[1]);
	fftw_destroy_plan(state->plan_r2c_left_c0);
	fftw_destroy_plan(state->plan_r2c_left_c1);
	fftw_destroy_plan(state->plan_c2r_left_c0);
	fftw_destroy_plan(state->plan_c2r_left_c1);
	fftw_destroy_plan(state->plan_r2c_right_c0);
	fftw_destroy_plan(state->plan_r2c_right_c1);
	fftw_destroy_plan(state->plan_c2r_right_c0);
	fftw_destroy_plan(state->plan_c2r_right_c1);
	free(state);
	free(e->channel_bit_array);
}

struct effect * crossfeed_hrtf_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_bit_array, int argc, char **argv)
{
	struct effect *e;
	struct crossfeed_hrtf_state *state;
	struct codec *c_left, *c_right;
	sample_t *tmp_buf;
	int i;
	ssize_t frames;
	fftw_plan impulse_plan0, impulse_plan1;
	sample_t *impulse[2];

	if (argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (istream->channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: channels != 2\n", argv[0]);
		return NULL;
	}
	c_left = init_codec(NULL, CODEC_MODE_READ, argv[1], NULL, CODEC_ENDIAN_DEFAULT, 0, 0);
	c_right = init_codec(NULL, CODEC_MODE_READ, argv[2], NULL, CODEC_ENDIAN_DEFAULT, 0, 0);
	if (c_left == NULL || c_right == NULL) {
		LOG(LL_ERROR, "dsp: %s: error: failed to open impulse file: %s\n", argv[0], argv[1]);
		return NULL;
	}
	if (c_left->channels != 2 || c_right->channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: impulse channels != 2\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	if (c_left->fs != istream->fs || c_right->fs != istream->fs) {
		LOG(LL_ERROR, "dsp: %s: error: sample rate mismatch\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	if (c_left->frames <= 1 || c_right->frames <= 1) {
		LOG(LL_ERROR, "dsp: %s: error: impulse length must > 1 sample\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	frames = (c_left->frames > c_right->frames) ? c_left->frames : c_right->frames;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_bit_array = NEW_BIT_ARRAY(istream->channels);
	COPY_BIT_ARRAY(e->channel_bit_array, channel_bit_array, istream->channels);
	e->ratio = 1.0;
	e->run = crossfeed_hrtf_effect_run;
	e->reset = crossfeed_hrtf_effect_reset;
	e->plot = NULL;
	e->drain = crossfeed_hrtf_effect_drain;
	e->destroy = crossfeed_hrtf_effect_destroy;

	state = calloc(1, sizeof(struct crossfeed_hrtf_state));
	state->input_frames = (frames - 1) * 2;
	state->fr_frames = frames;
	state->filter_fr_left[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_left[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_right[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_right[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->tmp_fr[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->tmp_fr[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->output_left_input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->output_left_input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->output_right_input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->output_right_input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->overlap_left_input[0] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_left_input[1] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_right_input[0] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_right_input[1] = calloc(state->input_frames / 2, sizeof(sample_t));

	impulse[0] = calloc(state->input_frames, sizeof(sample_t));
	impulse[1] = calloc(state->input_frames, sizeof(sample_t));
	tmp_buf = calloc(frames * 2, sizeof(sample_t));

	impulse_plan0 = fftw_plan_dft_r2c_1d(state->input_frames, impulse[0], state->filter_fr_left[0], FFTW_ESTIMATE);
	impulse_plan1 = fftw_plan_dft_r2c_1d(state->input_frames, impulse[1], state->filter_fr_left[1], FFTW_ESTIMATE);
	if (c_left->read(c_left, tmp_buf, c_left->frames) != c_left->frames)
		LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	for (i = 0; i < c_left->frames; ++i) {
		impulse[0][i] = tmp_buf[i * 2];
		impulse[1][i] = tmp_buf[i * 2 + 1];
	}
	fftw_execute(impulse_plan0);
	fftw_execute(impulse_plan1);
	fftw_destroy_plan(impulse_plan0);
	fftw_destroy_plan(impulse_plan1);

	memset(tmp_buf, 0, frames * 2 * sizeof(sample_t));
	memset(impulse[0], 0, state->input_frames * sizeof(sample_t));
	memset(impulse[1], 0, state->input_frames * sizeof(sample_t));
	impulse_plan0 = fftw_plan_dft_r2c_1d(state->input_frames, impulse[0], state->filter_fr_right[0], FFTW_ESTIMATE);
	impulse_plan1 = fftw_plan_dft_r2c_1d(state->input_frames, impulse[1], state->filter_fr_right[1], FFTW_ESTIMATE);
	if (c_right->read(c_right, tmp_buf, c_right->frames) != c_right->frames)
		LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	for (i = 0; i < c_right->frames; ++i) {
		impulse[0][i] = tmp_buf[i * 2];
		impulse[1][i] = tmp_buf[i * 2 + 1];
	}
	fftw_execute(impulse_plan0);
	fftw_execute(impulse_plan1);
	fftw_destroy_plan(impulse_plan0);
	fftw_destroy_plan(impulse_plan1);
	free(tmp_buf);
	free(impulse[0]);
	free(impulse[1]);

	/* init left input plans */
	state->plan_r2c_left_c0 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[0], state->tmp_fr[0], FFTW_ESTIMATE);
	state->plan_r2c_left_c1 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[0], state->tmp_fr[1], FFTW_ESTIMATE);
	state->plan_c2r_left_c0 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[0], state->output_left_input[0], FFTW_ESTIMATE);
	state->plan_c2r_left_c1 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[1], state->output_left_input[1], FFTW_ESTIMATE);

	/* init right input plans */
	state->plan_r2c_right_c0 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[1], state->tmp_fr[0], FFTW_ESTIMATE);
	state->plan_r2c_right_c1 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[1], state->tmp_fr[1], FFTW_ESTIMATE);
	state->plan_c2r_right_c0 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[0], state->output_right_input[0], FFTW_ESTIMATE);
	state->plan_c2r_right_c1 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[1], state->output_right_input[1], FFTW_ESTIMATE);

	LOG(LL_VERBOSE, "dsp: %s: impulse frames=%zd\n", argv[0], frames);
	destroy_codec(c_left);
	destroy_codec(c_right);

	e->data = state;
	return e;
}
