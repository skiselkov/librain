/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <acfutils/assert.h>
#include <acfutils/log.h>
#include <acfutils/helpers.h>
#include <acfutils/safe_alloc.h>

#include "obj8.h"

/*
 * Apparently these are the standard vertex attribute indices:
 * gl_Vertex		0
 * gl_Normal		2
 * gl_Color		3
 * gl_SecondaryColor	4
 * gl_FogCoord		5
 * gl_MultiTexCoord0	8
 * gl_MultiTexCoord1	9
 * gl_MultiTexCoord2	10
 * gl_MultiTexCoord3	11
 * gl_MultiTexCoord4	12
 * gl_MultiTexCoord5	13
 * gl_MultiTexCoord6	14
 * gl_MultiTexCoord7	15
 */
enum {
	VTX_ATTRIB_POS =	0,
	VTX_ATTRIB_NORM =	2,
	VTX_ATTRIB_TEX0 =	8
};

typedef struct {
	vect3_t		pos;
	vect3_t		norm;
	vect2_t		tex;
} obj8_vtx_t;

static obj8_t *
obj8_parse_fp(FILE *fp, const char *filename, vect3_t pos_offset)
{
	obj8_t		*obj = safe_calloc(1, sizeof (*obj));
	obj8_vtx_t	*vtx_table = NULL;
	unsigned	*idx_table = NULL;
	unsigned	vtx_cap = 0;
	unsigned	idx_cap = 0;
	char		*line = NULL;
	size_t		cap = 0;
	unsigned	cur_vtx = 0;
	unsigned	cur_idx = 0;
	char		group_id[32] = { 0 };
	bool_t		double_sided = B_FALSE;

	(void) glGetError();

	list_create(&obj->geom, sizeof (obj8_geom_t),
	    offsetof(obj8_geom_t, node));

	for (int linenr = 1; getline(&line, &cap, fp) > 0; linenr++) {
		strip_space(line);

		if (strncmp(line, "VT", 2) == 0) {
			obj8_vtx_t *vtx;
			if (cur_vtx >= vtx_cap) {
				logMsg("%s:%d: too many VT lines found",
				    filename, linenr);
				goto errout;
			}
			vtx = &vtx_table[cur_vtx];
			if (sscanf(line, "VT %lf %lf %lf %lf %lf %lf %lf %lf",
			    &vtx->pos.x, &vtx->pos.y, &vtx->pos.z,
			    &vtx->norm.x, &vtx->norm.y, &vtx->norm.z,
			    &vtx->tex.x, &vtx->tex.y) != 8) {
				logMsg("%s:%d: parsing of VT line failed",
				    filename, linenr);
				goto errout;
			}
			cur_vtx++;
		} else if (strncmp(line, "IDX10", 5) == 0) {
			if (cur_idx + 10 > idx_cap) {
				logMsg("%s:%d: too many IDX10 lines found",
				    filename, linenr);
				goto errout;
			}
			if (sscanf(line, "IDX10 %u %u %u %u %u %u %u %u %u %u",
			    &idx_table[cur_idx + 0], &idx_table[cur_idx + 1],
			    &idx_table[cur_idx + 2], &idx_table[cur_idx + 3],
			    &idx_table[cur_idx + 4], &idx_table[cur_idx + 5],
			    &idx_table[cur_idx + 6], &idx_table[cur_idx + 7],
			    &idx_table[cur_idx + 8], &idx_table[cur_idx + 9]) !=
			    10) {
				logMsg("%s:%d: parsing of IDX10 line failed",
				    filename, linenr);
				goto errout;
			}
			for (int i = 0; i < 10; i++) {
				if (idx_table[cur_idx + i] >= vtx_cap) {
					logMsg("%s:%d: index entry %d falls "
					    "outside of vertex table",
					    filename, linenr, i);
					goto errout;
				}
			}
			cur_idx += 10;
		} else if (strncmp(line, "IDX", 3) == 0) {
			if (cur_idx >= idx_cap) {
				logMsg("%s:%d: too many IDX lines found",
				    filename, linenr);
				goto errout;
			}
			if (sscanf(line, "IDX %u", &idx_table[cur_idx]) != 1) {
				logMsg("%s:%d: parsing of IDX line failed",
				    filename, linenr);
				goto errout;
			}
			if (idx_table[cur_idx] >= vtx_cap) {
				logMsg("%s:%d: index entry falls outside of "
				    "vertex table", filename, linenr);
			}
			cur_idx++;
		} else if (strncmp(line, "TRIS", 4) == 0) {
			obj8_geom_t *geom;
			unsigned off, len;

			if (sscanf(line, "TRIS %u %u", &off, &len) != 2) {
				logMsg("%s:%d: parsing of TRIS line failed",
				    filename, linenr);
				goto errout;
			}
			if (off + len > idx_cap) {
				logMsg("%s:%d: index table offsets are invalid",
				    filename, linenr);
				goto errout;
			}
			geom = safe_calloc(1, sizeof (*geom));
			geom->n_vtx = len;
			geom->vtx_pos = safe_calloc(len * 3,
			    sizeof (*geom->vtx_pos));
			geom->vtx_norm = safe_calloc(len * 3,
			    sizeof (*geom->vtx_norm));
			geom->vtx_tex = safe_calloc(len * 2,
			    sizeof (*geom->vtx_tex));
			strncpy(geom->group_id, group_id,
			    sizeof (geom->group_id));
			geom->double_sided = double_sided;

			for (unsigned i = 0; i < len; i++) {
				geom->vtx_pos[i * 3 + 0] = pos_offset.x +
				    vtx_table[idx_table[off + i]].pos.x;
				geom->vtx_pos[i * 3 + 1] = pos_offset.y +
				    vtx_table[idx_table[off + i]].pos.y;
				geom->vtx_pos[i * 3 + 2] = pos_offset.z +
				    vtx_table[idx_table[off + i]].pos.z;

				geom->vtx_norm[i * 3 + 0] =
				    vtx_table[idx_table[off + i]].norm.x;
				geom->vtx_norm[i * 3 + 1] =
				    vtx_table[idx_table[off + i]].norm.y;
				geom->vtx_norm[i * 3 + 2] =
				    vtx_table[idx_table[off + i]].norm.z;

				geom->vtx_tex[i * 2 + 0] =
				    vtx_table[idx_table[off + i]].tex.x;
				geom->vtx_tex[i * 2 + 1] =
				    vtx_table[idx_table[off + i]].tex.y;
			}

			glGenBuffers(1, &geom->vtx_pos_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBindBuffer(GL_ARRAY_BUFFER, geom->vtx_pos_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBufferData(GL_ARRAY_BUFFER,
			    3 * len * sizeof (GLfloat), geom->vtx_pos,
			    GL_STATIC_DRAW);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);

			glGenBuffers(1, &geom->vtx_norm_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBindBuffer(GL_ARRAY_BUFFER, geom->vtx_norm_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBufferData(GL_ARRAY_BUFFER,
			    3 * len * sizeof (GLfloat), geom->vtx_norm,
			    GL_STATIC_DRAW);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);

			glGenBuffers(1, &geom->vtx_tex_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBindBuffer(GL_ARRAY_BUFFER, geom->vtx_tex_buf);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);
			glBufferData(GL_ARRAY_BUFFER,
			    2 * len * sizeof (GLfloat), geom->vtx_tex,
			    GL_STATIC_DRAW);
			ASSERT3U(glGetError(), ==, GL_NO_ERROR);

			glBindBuffer(GL_ARRAY_BUFFER, 0);

			list_insert_tail(&obj->geom, geom);
		} else if (strncmp(line, "POINT_COUNTS", 12) == 0) {
			unsigned lines, lites;

			if (vtx_table != NULL) {
				logMsg("%s:%d: duplicate POINT_COUNTS line "
				    "found", filename, linenr);
				goto errout;
			}
			if (sscanf(line, "POINT_COUNTS %u %u %u %u",
			    &vtx_cap, &lines, &lites, &idx_cap) != 4) {
				logMsg("%s:%d: parsing of POINT_COUNTS failed",
				    filename, linenr);
				goto errout;
			}
			vtx_table = safe_calloc(vtx_cap, sizeof (*vtx_table));
			idx_table = safe_calloc(idx_cap, sizeof (*idx_table));
		} else if (strncmp(line, "X-GROUP-ID", 10) == 0) {
			if (sscanf(line, "X-GROUP-ID %31s", group_id) != 1)
				*group_id = 0;
		} else if (strncmp(line, "X-DOUBLE-SIDED", 14) == 0) {
			double_sided = B_TRUE;
		} else if (strncmp(line, "X-SINGLE-SIDED", 14) == 0) {
			double_sided = B_FALSE;
		}
	}

	free(vtx_table);
	free(idx_table);
	free(line);

	return (obj);
errout:
	free(vtx_table);
	free(idx_table);
	free(line);
	obj8_free(obj);

	return (NULL);
}

obj8_t *
obj8_parse(const char *filename, vect3_t pos_offset)
{
	FILE *fp = fopen(filename, "rb");
	obj8_t *obj;

	if (fp == NULL) {
		logMsg("Can't open %s: %s", filename, strerror(errno));
		return (NULL);
	}
	obj = obj8_parse_fp(fp, filename, pos_offset);
	fclose(fp);

	return (obj);
}

void
obj8_free(obj8_t *obj)
{
	obj8_geom_t *geom;

	while ((geom = list_remove_head(&obj->geom)) != NULL) {
		free(geom->vtx_pos);
		free(geom->vtx_norm);
		free(geom->vtx_tex);
		if (geom->vtx_pos_buf != 0)
			glDeleteBuffers(1, &geom->vtx_pos_buf);
		if (geom->vtx_norm_buf != 0)
			glDeleteBuffers(1, &geom->vtx_norm_buf);
		if (geom->vtx_tex_buf != 0)
			glDeleteBuffers(1, &geom->vtx_tex_buf);
		free(geom);
	}
	list_destroy(&obj->geom);

	free(obj);
}

static void
geom_buf_impl(GLuint attr_idx, GLuint buf, GLuint size, GLuint stride)
{
	glBindBuffer(GL_ARRAY_BUFFER, buf);
	glEnableVertexAttribArray(attr_idx);
	glVertexAttribPointer(attr_idx, size, GL_FLOAT, GL_FALSE, stride, NULL);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void
geom_buf_pos(const obj8_geom_t *geom)
{
	geom_buf_impl(VTX_ATTRIB_POS, geom->vtx_pos_buf, 3, 0);
}

static void
geom_buf_norm(const obj8_geom_t *geom)
{
	geom_buf_impl(VTX_ATTRIB_NORM, geom->vtx_norm_buf, 3, 0);
}

static void
geom_buf_tex0(const obj8_geom_t *geom)
{
	geom_buf_impl(VTX_ATTRIB_TEX0, geom->vtx_tex_buf, 2, 0);
}

static void
geom_draw(const obj8_geom_t *geom)
{
	geom_buf_pos(geom);
	geom_buf_norm(geom);
	geom_buf_tex0(geom);
	glDrawArrays(GL_TRIANGLES, 0, geom->n_vtx);
}

void
obj8_draw_group(const obj8_t *obj, const char *groupname)
{
	for (obj8_geom_t *geom = list_head(&obj->geom); geom != NULL;
	    geom = list_next(&obj->geom, geom)) {
		if (strcmp(geom->group_id, groupname) == 0) {
			if (geom->double_sided) {
				glCullFace(GL_FRONT);
				geom_draw(geom);
				glCullFace(GL_BACK);
			}
			geom_draw(geom);
		}
	}
	glDisableVertexAttribArray(VTX_ATTRIB_POS);
	glDisableVertexAttribArray(VTX_ATTRIB_NORM);
	glDisableVertexAttribArray(VTX_ATTRIB_TEX0);
}
