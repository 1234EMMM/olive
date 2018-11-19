#ifndef SOLIDEFFECT_H
#define SOLIDEFFECT_H

#include "project/effect.h"

class QOpenGLTexture;
#include <QImage>

class SolidEffect : public Effect {
	Q_OBJECT
public:
	SolidEffect(Clip* c, const EffectMeta *em);
	EffectField* solid_type;
	EffectField* solid_color_field;
	EffectField* opacity_field;
    EffectField* solid_width_field;
	void redraw(double timecode);
};

#endif // SOLIDEFFECT_H
