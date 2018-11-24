#include "transformeffect.h"

#include <QWidget>
#include <QLabel>
#include <QGridLayout>
#include <QSpinBox>
#include <QCheckBox>
#include <QOpenGLFunctions>
#include <QComboBox>

#include "ui/collapsiblewidget.h"
#include "project/clip.h"
#include "project/sequence.h"
#include "io/media.h"
#include "ui/labelslider.h"
#include "ui/comboboxex.h"
#include "panels/project.h"
#include "debug.h"

#define BLEND_MODE_NORMAL 0
#define BLEND_MODE_SCREEN 1
#define BLEND_MODE_MULTIPLY 2
#define BLEND_MODE_OVERLAY 3

TransformEffect::TransformEffect(Clip* c, const EffectMeta* em) : Effect(c, em) {
	enable_coords = true;

	EffectRow* position_row = add_row("Position:");
    position_x = position_row->add_field(EFFECT_FIELD_DOUBLE, "posx"); // position X
    position_y = position_row->add_field(EFFECT_FIELD_DOUBLE, "posy"); // position Y

	EffectRow* scale_row = add_row("Scale:");
    scale_x = scale_row->add_field(EFFECT_FIELD_DOUBLE, "scalex"); // scale X (and Y is uniform scale is selected)
	scale_x->set_double_minimum_value(0);
	scale_x->set_double_maximum_value(3000);
    scale_y = scale_row->add_field(EFFECT_FIELD_DOUBLE, "scaley"); // scale Y (disabled if uniform scale is selected)
	scale_y->set_double_minimum_value(0);
	scale_y->set_double_maximum_value(3000);

	EffectRow* uniform_scale_row = add_row("Uniform Scale:");
    uniform_scale_field = uniform_scale_row->add_field(EFFECT_FIELD_BOOL, "uniformscale"); // uniform scale option

	EffectRow* rotation_row = add_row("Rotation:");
    rotation = rotation_row->add_field(EFFECT_FIELD_DOUBLE, "rotation");

	EffectRow* anchor_point_row = add_row("Anchor Point:");
    anchor_x_box = anchor_point_row->add_field(EFFECT_FIELD_DOUBLE, "anchorx"); // anchor point X
    anchor_y_box = anchor_point_row->add_field(EFFECT_FIELD_DOUBLE, "anchory"); // anchor point Y

	EffectRow* opacity_row = add_row("Opacity:");
    opacity = opacity_row->add_field(EFFECT_FIELD_DOUBLE, "opacity"); // opacity
	opacity->set_double_minimum_value(0);
	opacity->set_double_maximum_value(100);

	EffectRow* blend_mode_row = add_row("Blend Mode:");
    blend_mode_box = blend_mode_row->add_field(EFFECT_FIELD_COMBO, "blendmode"); // blend mode
	blend_mode_box->add_combo_item("Normal", BLEND_MODE_NORMAL);
	blend_mode_box->add_combo_item("Overlay", BLEND_MODE_OVERLAY);
	blend_mode_box->add_combo_item("Screen", BLEND_MODE_SCREEN);
	blend_mode_box->add_combo_item("Multiply", BLEND_MODE_MULTIPLY);

	// set defaults
	scale_y->set_enabled(false);
	uniform_scale_field->set_bool_value(true);
	blend_mode_box->set_combo_index(0);
	refresh();

	connect(position_x, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(position_y, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(scale_x, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(scale_y, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(uniform_scale_field, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(rotation, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(anchor_x_box, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(anchor_y_box, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(opacity, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(blend_mode_box, SIGNAL(changed()), this, SLOT(field_changed()));
	connect(uniform_scale_field, SIGNAL(toggled(bool)), this, SLOT(toggle_uniform_scale(bool)));
}

void TransformEffect::refresh() {
	if (parent_clip->sequence != NULL) {
        double default_pos_x = parent_clip->sequence->width/2;
        double default_pos_y = parent_clip->sequence->height/2;

		position_x->set_double_default_value(default_pos_x);
		position_y->set_double_default_value(default_pos_y);
		scale_x->set_double_default_value(100);
		scale_y->set_double_default_value(100);

		default_anchor_x = parent_clip->getWidth()/2;
		default_anchor_y = parent_clip->getHeight()/2;

		if (default_anchor_x == 0) default_anchor_x = default_pos_x;
		if (default_anchor_y == 0) default_anchor_y = default_pos_y;

		anchor_x_box->set_double_default_value(default_anchor_x);
		anchor_y_box->set_double_default_value(default_anchor_y);
		opacity->set_double_default_value(100);
	}
}

void TransformEffect::toggle_uniform_scale(bool enabled) {
	scale_y->set_enabled(!enabled);
}

void TransformEffect::process_coords(double timecode, GLTextureCoords& coords, int data) {
	// position
	glTranslatef(position_x->get_double_value(timecode)-(parent_clip->sequence->width/2), position_y->get_double_value(timecode)-(parent_clip->sequence->height/2), 0);

	// anchor point
	int anchor_x_offset = (anchor_x_box->get_double_value(timecode)-default_anchor_x);
	int anchor_y_offset = (anchor_y_box->get_double_value(timecode)-default_anchor_y);
	coords.vertexTopLeftX -= anchor_x_offset;
	coords.vertexTopRightX -= anchor_x_offset;
	coords.vertexBottomLeftX -= anchor_x_offset;
	coords.vertexBottomRightX -= anchor_x_offset;
	coords.vertexTopLeftY -= anchor_y_offset;
	coords.vertexTopRightY -= anchor_y_offset;
	coords.vertexBottomLeftY -= anchor_y_offset;
	coords.vertexBottomRightY -= anchor_y_offset;

	// rotation
    glRotatef(rotation->get_double_value(timecode), 0, 0, 1);

	// scale
	float sx = scale_x->get_double_value(timecode)*0.01;
	float sy = (uniform_scale_field->get_bool_value(timecode)) ? sx : scale_y->get_double_value(timecode)*0.01;
    glScalef(sx, sy, 1);

    // blend mode
	switch (blend_mode_box->get_combo_data(timecode).toInt()) {
    case BLEND_MODE_NORMAL:
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BLEND_MODE_OVERLAY:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case BLEND_MODE_SCREEN:
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
        break;
    case BLEND_MODE_MULTIPLY:
		glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
        break;
    default:
		dout << "[ERROR] Invalid blend mode. This is a bug - please contact developers";
    }

	// opacity
    float color[4];
    glGetFloatv(GL_CURRENT_COLOR, color);
	glColor4f(1.0, 1.0, 1.0, color[3]*(opacity->get_double_value(timecode)*0.01));
}
