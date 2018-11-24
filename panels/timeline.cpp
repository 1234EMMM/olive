#include "timeline.h"
#include "ui_timeline.h"

#include "panels/panels.h"
#include "panels/project.h"
#include "panels/effectcontrols.h"
#include "ui/timelinewidget.h"
#include "project/sequence.h"
#include "project/clip.h"
#include "ui/viewerwidget.h"
#include "playback/audio.h"
#include "panels/viewer.h"
#include "playback/cacher.h"
#include "playback/playback.h"
#include "ui_viewer.h"
#include "project/undo.h"
#include "io/config.h"
#include "project/effect.h"
#include "project/transition.h"
#include "io/media.h"
#include "debug.h"

#include <QTime>
#include <QScrollBar>
#include <QtMath>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>

long refactor_frame_number(long framenumber, double source_frame_rate, double target_frame_rate) {
	return qRound(((double)framenumber/source_frame_rate)*target_frame_rate);
}

void draw_selection_rectangle(QPainter& painter, const QRect& rect) {
	painter.setPen(QColor(204, 204, 204));
	painter.setBrush(QColor(0, 0, 0, 32));
	painter.drawRect(rect);
}

Timeline::Timeline(QWidget *parent) :
	QDockWidget(parent),
    cursor_frame(0),
    cursor_track(0),
    zoom(1.0),
    showing_all(false),
    snapping(true),
    snapped(false),
    snap_point(0),
    selecting(false),
    rect_select_init(false),
    rect_select_proc(false),
    moving_init(false),
    moving_proc(false),
    move_insert(false),
    trim_target(-1),
    trim_in_point(false),
    splitting(false),
    importing(false),
	importing_files(false),
    creating(false),
    transition_tool_init(false),
    transition_tool_proc(false),
    transition_tool_pre_clip(-1),
    transition_tool_post_clip(-1),
    ui(new Ui::Timeline),
	last_frame(0),	
    scroll(0)
{
	default_track_height = (QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96) * TRACK_DEFAULT_HEIGHT;

	ui->setupUi(this);

	ui->headers->viewer = panel_sequence_viewer;

    /* --- TEMPORARY ---
     * I feel like the rolling edit tool is unnecessary, but just
     * in case I change my mind, I'm leaving all the functionality
     * intact and only hiding the UI to get to it. Still deciding
     * on this one but the pointer tool literally does the same
     * functionality.
     */
    ui->toolRollingButton->setVisible(false);
    ui->toolRollingButton->setEnabled(false);
    /* --- */

	ui->video_area->bottom_align = true;
	ui->video_area->scrollBar = ui->videoScrollbar;
	ui->audio_area->scrollBar = ui->audioScrollbar;

	tool_buttons.append(ui->toolArrowButton);
	tool_buttons.append(ui->toolEditButton);
	tool_buttons.append(ui->toolRippleButton);
	tool_buttons.append(ui->toolRazorButton);
	tool_buttons.append(ui->toolSlipButton);
	tool_buttons.append(ui->toolRollingButton);
    tool_buttons.append(ui->toolSlideButton);
	tool_buttons.append(ui->toolTransitionButton);

	ui->toolArrowButton->click();

	connect(ui->horizontalScrollBar, SIGNAL(valueChanged(int)), this, SLOT(setScroll(int)));
	connect(ui->videoScrollbar, SIGNAL(valueChanged(int)), ui->video_area, SLOT(setScroll(int)));
	connect(ui->audioScrollbar, SIGNAL(valueChanged(int)), ui->audio_area, SLOT(setScroll(int)));

	update_sequence();
}

Timeline::~Timeline()
{
	delete ui;
}

void Timeline::previous_cut() {
	if (sequence->playhead > 0) {
        long p_cut = 0;
        for (int i=0;i<sequence->clips.size();i++) {
            Clip* c = sequence->clips.at(i);
            if (c != NULL) {
				if (c->timeline_out > p_cut && c->timeline_out < sequence->playhead) {
                    p_cut = c->timeline_out;
				} else if (c->timeline_in > p_cut && c->timeline_in < sequence->playhead) {
                    p_cut = c->timeline_in;
                }
            }
        }
		panel_sequence_viewer->seek(p_cut);
    }
}

void Timeline::next_cut() {
    bool seek_enabled = false;
    long n_cut = LONG_MAX;
    for (int i=0;i<sequence->clips.size();i++) {
        Clip* c = sequence->clips.at(i);
        if (c != NULL) {
			if (c->timeline_in < n_cut && c->timeline_in > sequence->playhead) {
                n_cut = c->timeline_in;
                seek_enabled = true;
			} else if (c->timeline_out < n_cut && c->timeline_out > sequence->playhead) {
                n_cut = c->timeline_out;
                seek_enabled = true;
            }
        }
    }
	if (seek_enabled) panel_sequence_viewer->seek(n_cut);
}

void Timeline::toggle_show_all() {
	showing_all = !showing_all;
	if (showing_all) {
		old_zoom = zoom;
		set_zoom_value((double) (ui->timeline_area->width() - 200) / (double) sequence->getEndFrame());
	} else {
		set_zoom_value(old_zoom);
	}
}

void Timeline::create_ghosts_from_media(Sequence* seq, long entry_point, QVector<void*>& media_list, QVector<int>& type_list) {
	video_ghosts = false;
	audio_ghosts = false;

	for (int i=0;i<media_list.size();i++) {
		bool can_import = true;

		Media* m = NULL;
		Sequence* s = NULL;
		void* media = NULL;
		long sequence_length;
		long default_clip_in = 0;
		long default_clip_out = 0;

		switch (type_list.at(i)) {
		case MEDIA_TYPE_FOOTAGE:
			m = static_cast<Media*>(media_list.at(i));
			media = m;
			can_import = m->ready;
			if (m->using_inout) {
				double source_fr = 30;
				if (m->video_tracks.size() > 0) source_fr = m->video_tracks.at(0)->video_frame_rate;
				default_clip_in = refactor_frame_number(m->in, source_fr, seq->frame_rate);
				default_clip_out = refactor_frame_number(m->out, source_fr, seq->frame_rate);
			}
			break;
		case MEDIA_TYPE_SEQUENCE:
			s = static_cast<Sequence*>(media_list.at(i));
			sequence_length = s->getEndFrame();
			if (seq != NULL) sequence_length = refactor_frame_number(sequence_length, s->frame_rate, seq->frame_rate);
			media = s;
			can_import = (s != seq && sequence_length != 0);
			if (s->using_workarea) {
				default_clip_in = refactor_frame_number(s->workarea_in, s->frame_rate, seq->frame_rate);
				default_clip_out = refactor_frame_number(s->workarea_out, s->frame_rate, seq->frame_rate);
			}
			break;
		default:
			can_import = false;
		}

		if (can_import) {
			Ghost g;
			g.clip = -1;
			g.media_type = type_list.at(i);
			g.trimming = false;
			g.old_clip_in = g.clip_in = default_clip_in;
			g.media = media;
			g.in = entry_point;
			g.transition = NULL;

			switch (type_list.at(i)) {
			case MEDIA_TYPE_FOOTAGE:
				// is video source a still image?
				if (m->video_tracks.size() > 0 && m->video_tracks[0]->infinite_length && m->audio_tracks.size() == 0) {
					g.out = g.in + 100;
				} else {
					long length = m->get_length_in_frames(seq->frame_rate);
					g.out = entry_point + length - default_clip_in;
					if (m->using_inout) {
						g.out -= (length - default_clip_out);
					}
				}

				for (int j=0;j<m->audio_tracks.size();j++) {
					g.track = j;
					g.media_stream = m->audio_tracks.at(j)->file_index;
					ghosts.append(g);
					audio_ghosts = true;
				}
				for (int j=0;j<m->video_tracks.size();j++) {
					g.track = -1-j;
					g.media_stream = m->video_tracks.at(j)->file_index;
					ghosts.append(g);
					video_ghosts = true;
				}
				break;
			case MEDIA_TYPE_SEQUENCE:
				g.out = entry_point + sequence_length - default_clip_in;

				if (s->using_workarea) {
					g.out -= (sequence_length - default_clip_out);
				}

				g.track = -1;
				ghosts.append(g);
				g.track = 0;
				ghosts.append(g);

				video_ghosts = true;
				audio_ghosts = true;
				break;
			}
			entry_point = g.out;
		}
	}
	for (int i=0;i<ghosts.size();i++) {
		Ghost& g = ghosts[i];
		g.old_in = g.in;
		g.old_out = g.out;
		g.old_track = g.track;
	}
}

void Timeline::add_clips_from_ghosts(ComboAction* ca, Sequence* s) {
	// add clips
	long earliest_point = LONG_MAX;
	QVector<Clip*> added_clips;
	for (int i=0;i<ghosts.size();i++) {
		const Ghost& g = ghosts.at(i);

		earliest_point = qMin(earliest_point, g.in);

		Clip* c = new Clip(s);
		c->media = g.media;
		c->media_type = g.media_type;
		c->media_stream = g.media_stream;
		c->timeline_in = g.in;
		c->timeline_out = g.out;
		c->clip_in = g.clip_in;
		c->track = g.track;
		if (c->media_type == MEDIA_TYPE_FOOTAGE) {
			Media* m = static_cast<Media*>(c->media);
			if (m->video_tracks.size() == 0) {
				// audio only (greenish)
				c->color_r = 128;
				c->color_g = 192;
				c->color_b = 128;
			} else if (m->audio_tracks.size() == 0) {
				// video only (orangeish)
				c->color_r = 192;
				c->color_g = 160;
				c->color_b = 128;
			} else {
				// video and audio (blueish)
				c->color_r = 128;
				c->color_g = 128;
				c->color_b = 192;
			}
			c->name = m->name;
		} else if (c->media_type == MEDIA_TYPE_SEQUENCE) {
			// sequence (red?ish?)
			c->color_r = 192;
			c->color_g = 128;
			c->color_b = 128;

			Sequence* media = static_cast<Sequence*>(c->media);
			c->name = media->name;
		}
		c->recalculateMaxLength();
		added_clips.append(c);
	}
	ca->append(new AddClipCommand(s, added_clips));

	// link clips from the same media
	for (int i=0;i<added_clips.size();i++) {
		Clip* c = added_clips.at(i);
		for (int j=0;j<added_clips.size();j++) {
			Clip* cc = added_clips.at(j);
			if (c != cc && c->media == cc->media) {
				c->linked.append(j);
			}
		}

		if (c->track < 0) {
			// add default video effects
			c->effects.append(create_effect(c, get_internal_meta(EFFECT_INTERNAL_TRANSFORM)));
		} else {
			// add default audio effects
			c->effects.append(create_effect(c, get_internal_meta(EFFECT_INTERNAL_VOLUME)));
			c->effects.append(create_effect(c, get_internal_meta(EFFECT_INTERNAL_PAN)));
		}
	}
	if (config.enable_seek_to_import) {
		panel_sequence_viewer->seek(earliest_point);
	}
	panel_timeline->ghosts.clear();
	panel_timeline->importing = false;
	panel_timeline->snapped = false;
}

int Timeline::get_track_height_size(bool video) {
    if (video) {
        return video_track_heights.size();
    } else {
        return audio_track_heights.size();
    }
}

void Timeline::add_transition() {
    ComboAction* ca = new ComboAction();
	bool adding = false;

    for (int i=0;i<sequence->clips.size();i++) {
        Clip* c = sequence->clips.at(i);
        if (c != NULL && is_clip_selected(c, true)) {
            if (c->get_opening_transition() == NULL) {
                ca->append(new AddTransitionCommand(c, NULL, get_internal_meta(TRANSITION_INTERNAL_LINEARFADE), TA_OPENING_TRANSITION, 30));
				adding = true;
            }
            if (c->get_closing_transition() == NULL) {
                ca->append(new AddTransitionCommand(c, NULL, get_internal_meta(TRANSITION_INTERNAL_LINEARFADE), TA_OPENING_TRANSITION, 30));
				adding = true;
            }
        }
    }

	if (adding) {
        undo_stack.push(ca);
	} else {
        delete ca;
	}

	update_ui(true);
}

int Timeline::calculate_track_height(int track, int value) {
    int index = (track < 0) ? qAbs(track + 1) : track;
    QVector<int>& vector = (track < 0) ? video_track_heights : audio_track_heights;
    while (vector.size() < index+1) {
		vector.append(default_track_height);
    }
    if (value > -1) {
        vector[index] = value;
    }
    return vector.at(index);
}

void Timeline::update_sequence() {
    bool null_sequence = (sequence == NULL);

	for (int i=0;i<tool_buttons.count();i++) {
		tool_buttons[i]->setEnabled(!null_sequence);
	}
    ui->snappingButton->setEnabled(!null_sequence);
	ui->pushButton_4->setEnabled(!null_sequence);
	ui->pushButton_5->setEnabled(!null_sequence);
	ui->recordButton->setEnabled(!null_sequence);
	ui->addButton->setEnabled(!null_sequence);
    ui->headers->setEnabled(!null_sequence);

	if (null_sequence) {
		setWindowTitle("Timeline: <none>");
	} else {
		setWindowTitle("Timeline: " + sequence->name);
		update_ui(false);
	}
}

int Timeline::get_snap_range() {
	return getFrameFromScreenPoint(zoom, 10);
}

bool Timeline::focused() {
	return (sequence != NULL && (ui->headers->hasFocus() || ui->video_area->hasFocus() || ui->audio_area->hasFocus()));
}

bool Timeline::center_scroll_to_playhead() {
    // returns true is the scroll was changed, false if not
    int target_scroll = qMin(ui->horizontalScrollBar->maximum(), qMax(0, getScreenPointFromFrame(zoom, sequence->playhead)-(ui->editAreas->width()>>1)));
    if (target_scroll == ui->horizontalScrollBar->value()) {
        return false;
    }
    ui->horizontalScrollBar->setValue(target_scroll);
    return true;
}

void Timeline::repaint_timeline() {
    bool draw = true;

    if (sequence != NULL
            && !ui->horizontalScrollBar->isSliderDown()
            && panel_sequence_viewer->playing) {
        // auto scroll
        if (config.autoscroll == AUTOSCROLL_PAGE_SCROLL) {
            int playhead_x = panel_timeline->getTimelineScreenPointFromFrame(sequence->playhead);
            if (playhead_x < 0 || playhead_x > (ui->editAreas->width() - ui->videoScrollbar->width())) {
                ui->horizontalScrollBar->setValue(getScreenPointFromFrame(zoom, sequence->playhead));
                draw = false;
            }
        } else if (config.autoscroll == AUTOSCROLL_SMOOTH_SCROLL) {
            if (center_scroll_to_playhead()) {
                draw = false;
            }
        }
    }

    if (draw) {
        ui->headers->update();
        ui->video_area->update();
        ui->audio_area->update();

        if (sequence != NULL) {
            long sequenceEndFrame = sequence->getEndFrame();

            ui->horizontalScrollBar->setMaximum(qMax(0, getScreenPointFromFrame(zoom, sequenceEndFrame) - (ui->editAreas->width()/2)));

            if (last_frame != sequence->playhead) {
                ui->audio_monitor->update();
                last_frame = sequence->playhead;
            }
        }
    }
}

void Timeline::select_all() {
	if (sequence != NULL) {
		sequence->selections.clear();
        for (int i=0;i<sequence->clips.size();i++) {
            Clip* c = sequence->clips.at(i);
			if (c != NULL) {
				Selection s;
				s.in = c->timeline_in;
				s.out = c->timeline_out;
				s.track = c->track;
				sequence->selections.append(s);
			}
		}
		repaint_timeline();
	}
}

void Timeline::delete_in_out(bool ripple) {
    if (sequence != NULL && sequence->using_workarea) {
        QVector<Selection> areas;
        int video_tracks = 0, audio_tracks = 0;
        sequence->getTrackLimits(&video_tracks, &audio_tracks);
        for (int i=video_tracks;i<=audio_tracks;i++) {
            Selection s;
            s.in = sequence->workarea_in;
            s.out = sequence->workarea_out;
            s.track = i;
            areas.append(s);
        }
        ComboAction* ca = new ComboAction();
        delete_areas_and_relink(ca, areas);
        if (ripple) ca->append(new RippleCommand(sequence, sequence->workarea_in, sequence->workarea_in - sequence->workarea_out));
        ca->append(new SetTimelineInOutCommand(sequence, false, 0, 0));
        undo_stack.push(ca);
		update_ui(true);
    }
}

void Timeline::delete_selection(QVector<Selection>& selections, bool ripple_delete) {
	if (selections.size() > 0) {
        panel_effect_controls->clear_effects(true);

        ComboAction* ca = new ComboAction();

		delete_areas_and_relink(ca, selections);

		if (ripple_delete) {
			long ripple_point = selections.at(0).in;
			long ripple_length = selections.at(0).out - selections.at(0).in;

            // retrieve ripple_point and ripple_length from current selection
			for (int i=1;i<selections.size();i++) {
				const Selection& s = selections.at(i);
                ripple_point = qMin(ripple_point, s.in);
                ripple_length = qMin(ripple_length, s.out - s.in);
            }

            // it feels a little more intuitive with this here
            ripple_point++;

            bool can_ripple = true;
            for (int i=0;i<sequence->clips.size();i++) {
                Clip* c = sequence->clips.at(i);
				if (c != NULL && c->timeline_in < ripple_point && c->timeline_out > ripple_point) {
                    // conflict detected, but this clip may be getting deleted so let's check
                    bool deleted = false;
					for (int j=0;j<selections.size();j++) {
						const Selection& s = selections.at(j);
                        if (s.track == c->track
                                && !(c->timeline_in < s.in && c->timeline_out < s.in)
                                && !(c->timeline_in > s.out && c->timeline_out > s.out)) {
                            deleted = true;
                            break;
                        }
                    }
                    if (!deleted) {
                        for (int j=0;j<sequence->clips.size();j++) {
                            Clip* cc = sequence->clips.at(j);
							if (cc != NULL
									&& cc->track == c->track
                                    && cc->timeline_in > c->timeline_out
                                    && cc->timeline_in < c->timeline_out + ripple_length) {
                                ripple_length = cc->timeline_in - c->timeline_out;
                            }
                        }
                    }
                }
			}

			if (can_ripple) {
				ca->append(new RippleCommand(sequence, ripple_point, -ripple_length));
				panel_sequence_viewer->seek(ripple_point-1);
			}
		}

		selections.clear();

        undo_stack.push(ca);

		update_ui(true);
	}
}

void Timeline::set_zoom_value(double v) {
	zoom = v;
	ui->headers->update_zoom(zoom);
	repaint_timeline();

    // TODO find a way to gradually move towards target_scroll instead of just centering it?
    center_scroll_to_playhead();
}

void Timeline::set_zoom(bool in) {
	showing_all = false;
	set_zoom_value(zoom * ((in) ? 2 : 0.5));
}

void Timeline::decheck_tool_buttons(QObject* sender) {
	for (int i=0;i<tool_buttons.count();i++) {
        tool_buttons[i]->setChecked(tool_buttons.at(i) == sender);
	}
}

QVector<int> Timeline::get_tracks_of_linked_clips(int i) {
    QVector<int> tracks;
    Clip* clip = sequence->clips.at(i);
    for (int j=0;j<clip->linked.size();j++) {
        tracks.append(sequence->clips.at(clip->linked.at(j))->track);
    }
    return tracks;
}

void Timeline::on_pushButton_4_clicked()
{
    set_zoom(true);
}

void Timeline::on_pushButton_5_clicked()
{
    set_zoom(false);
}

bool Timeline::is_clip_selected(Clip* clip, bool containing) {
	for (int i=0;i<clip->sequence->selections.size();i++) {
		const Selection& s = clip->sequence->selections.at(i);
        if (clip->track == s.track && ((clip->timeline_in >= s.in && clip->timeline_out <= s.out && containing) ||
                (!containing && !(clip->timeline_in < s.in && clip->timeline_out < s.in) && !(clip->timeline_in > s.in && clip->timeline_out > s.in)))) {
			return true;
		}
	}
	return false;
}

void Timeline::on_snappingButton_toggled(bool checked) {
	snapping = checked;
}

Clip* Timeline::split_clip(ComboAction* ca, int p, long frame) {
    return split_clip(ca, p, frame, frame);
}

Clip* Timeline::split_clip(ComboAction* ca, int p, long frame, long post_in) {
    Clip* pre = sequence->clips.at(p);
    if (pre != NULL && pre->timeline_in < frame && pre->timeline_out > frame) { // guard against attempts to split at in/out points
        Clip* post = pre->copy(sequence);

		long new_clip_length = frame - pre->timeline_in;

		post->timeline_in = post_in;
		post->clip_in = pre->clip_in + (post->timeline_in - pre->timeline_in);

        ca->append(new MoveClipAction(pre, pre->timeline_in, frame, pre->clip_in, pre->track));

        if (pre->get_opening_transition() != NULL) {
            if (pre->get_opening_transition()->length > new_clip_length) {
                ca->append(new ModifyTransitionCommand(pre, TA_OPENING_TRANSITION, new_clip_length));
			}

            post->sequence->hard_delete_transition(post, TA_OPENING_TRANSITION);
		}
        if (pre->get_closing_transition() != NULL) {
            ca->append(new DeleteTransitionCommand(pre, TA_CLOSING_TRANSITION));

            post->get_closing_transition()->length = qMin((long) post->get_closing_transition()->length, post->getLength());
		}

        return post;
    }
    return NULL;
}

bool Timeline::has_clip_been_split(int c) {
    for (int i=0;i<split_cache.size();i++) {
        if (split_cache.at(i) == c) {
            return true;
        }
    }
    return false;
}

bool Timeline::split_clip_and_relink(ComboAction *ca, int clip, long frame, bool relink) {
    // see if we split this clip before
    if (has_clip_been_split(clip)) {
        return false;
    }

    split_cache.append(clip);

    Clip* c = sequence->clips.at(clip);
    if (c != NULL) {
        QVector<int> pre_clips;
        QVector<Clip*> post_clips;

        Clip* post = split_clip(ca, clip, frame);

        // if alt is not down, split clips links too
        if (post == NULL) {
            return false;
        } else {
            post_clips.append(post);
            if (relink) {
                pre_clips.append(clip);

                bool original_clip_is_selected = is_clip_selected(c, true);

                // find linked clips of old clip
                for (int i=0;i<c->linked.size();i++) {
                    int l = c->linked.at(i);
                    if (!has_clip_been_split(l)) {
                        Clip* link = sequence->clips.at(l);
                        if ((original_clip_is_selected && is_clip_selected(link, true)) || !original_clip_is_selected) {
                            split_cache.append(l);
                            Clip* s = split_clip(ca, l, frame);
                            if (s != NULL) {
                                pre_clips.append(l);
                                post_clips.append(s);
                            }
                        }
                    }
                }

                relink_clips_using_ids(pre_clips, post_clips);
            }
            ca->append(new AddClipCommand(sequence, post_clips));
            return true;
        }
    }
    return false;
}

void Timeline::clean_up_selections(QVector<Selection>& areas) {
    for (int i=0;i<areas.size();i++) {
        Selection& s = areas[i];
        for (int j=0;j<areas.size();j++) {
            if (i != j) {
                Selection& ss = areas[j];
                if (s.track == ss.track) {
                    bool remove = false;
                    if (s.in < ss.in && s.out > ss.out) {
                        // do nothing
                    } else if (s.in >= ss.in && s.out <= ss.out) {
                        remove = true;
                    } else if (s.in <= ss.out && s.out > ss.out) {
                        ss.out = s.out;
                        remove = true;
                    } else if (s.out >= ss.in && s.in < ss.in) {
                        ss.in = s.in;
                        remove = true;
                    }
                    if (remove) {
                        areas.removeAt(i);
                        i--;
                        break;
                    }
                }
            }
        }
    }
}

bool selection_contains_transition(const Selection& s, Clip* c, int type) {
    if (type == TA_OPENING_TRANSITION) {
        return c->get_opening_transition() != NULL
                && s.out == c->timeline_in + c->get_opening_transition()->length
                && ((c->get_opening_transition()->secondary_clip == NULL && s.in == c->timeline_in)
                || (c->get_opening_transition()->secondary_clip != NULL && s.in == c->timeline_in - c->get_opening_transition()->length));
    } else {
        return c->get_closing_transition() != NULL
                && s.in == c->timeline_out - c->get_closing_transition()->length
               && ((c->get_closing_transition()->secondary_clip == NULL && s.out == c->timeline_out)
               || (c->get_closing_transition()->secondary_clip != NULL && s.out == c->timeline_out + c->get_closing_transition()->length));
    }
}

void Timeline::delete_areas_and_relink(ComboAction* ca, QVector<Selection>& areas) {
    clean_up_selections(areas);

    QVector<int> pre_clips;
    QVector<Clip*> post_clips;

    for (int i=0;i<areas.size();i++) {
        const Selection& s = areas.at(i);
        for (int j=0;j<sequence->clips.size();j++) {
            Clip* c = sequence->clips.at(j);
            if (c != NULL && c->track == s.track && !c->undeletable) {
                if (selection_contains_transition(s, c, TA_OPENING_TRANSITION)) {
					// delete opening transition
                    ca->append(new DeleteTransitionCommand(c, TA_OPENING_TRANSITION));
                } else if (selection_contains_transition(s, c, TA_CLOSING_TRANSITION)) {
					// delete closing transition
                    ca->append(new DeleteTransitionCommand(c, TA_CLOSING_TRANSITION));
				} else if (c->timeline_in >= s.in && c->timeline_out <= s.out) {
                    // clips falls entirely within deletion area
                    ca->append(new DeleteClipAction(sequence, j));
                } else if (c->timeline_in < s.in && c->timeline_out > s.out) {
                    // middle of clip is within deletion area

                    // duplicate clip
                    Clip* post = split_clip(ca, j, s.in, s.out);

					pre_clips.append(j);
					post_clips.append(post);
                } else if (c->timeline_in < s.in && c->timeline_out > s.in) {
                    // only out point is in deletion area
                    ca->append(new MoveClipAction(c, c->timeline_in, s.in, c->clip_in, c->track));

                    if (c->get_closing_transition() != NULL) {
                        if (s.in < c->timeline_out - c->get_closing_transition()->length) {
                            ca->append(new DeleteTransitionCommand(c, TA_CLOSING_TRANSITION));
						} else {
                            ca->append(new ModifyTransitionCommand(c, TA_CLOSING_TRANSITION, c->get_closing_transition()->length - (c->timeline_out - s.in)));
						}
					}
                } else if (c->timeline_in < s.out && c->timeline_out > s.out) {
                    // only in point is in deletion area
                    ca->append(new MoveClipAction(c, s.out, c->timeline_out, c->clip_in + (s.out - c->timeline_in), c->track));

                    if (c->get_opening_transition() != NULL) {
                        if (s.out > c->timeline_in + c->get_opening_transition()->length) {
                            ca->append(new DeleteTransitionCommand(c, TA_OPENING_TRANSITION));
						} else {
                            ca->append(new ModifyTransitionCommand(c, TA_OPENING_TRANSITION, c->get_opening_transition()->length - (s.out - c->timeline_in)));
						}
					}
                }
            }
        }
    }
    relink_clips_using_ids(pre_clips, post_clips);
    ca->append(new AddClipCommand(sequence, post_clips));
}

void Timeline::copy(bool del) {
    bool cleared = false;
    bool copied = false;

    long min_in = 0;

    for (int i=0;i<sequence->clips.size();i++) {
        Clip* c = sequence->clips.at(i);
        if (c != NULL) {
			for (int j=0;j<sequence->selections.size();j++) {
				const Selection& s = sequence->selections.at(j);
                if (s.track == c->track && !((c->timeline_in <= s.in && c->timeline_out <= s.in) || (c->timeline_in >= s.out && c->timeline_out >= s.out))) {
                    if (!cleared) {
                        clip_clipboard.clear();
                        cleared = true;
                    }

                    Clip* copied_clip = c->copy(NULL);

                    // copy linked IDs (we correct these later in paste())
                    copied_clip->linked = c->linked;

                    if (copied_clip->timeline_in < s.in) {
                        copied_clip->clip_in += (s.in - copied_clip->timeline_in);
                        copied_clip->timeline_in = s.in;
                    }

                    if (copied_clip->timeline_out > s.out) {
                        copied_clip->timeline_out = s.out;
                    }

                    if (copied) {
                        min_in = qMin(min_in, s.in);
                    } else {
                        min_in = s.in;
                        copied = true;
                    }

                    copied_clip->load_id = i;

                    clip_clipboard.append(copied_clip);
                }
            }
        }
    }

    for (int i=0;i<clip_clipboard.size();i++) {
        // initialize all timeline_ins to 0 or offsets of
        clip_clipboard[i]->timeline_in -= min_in;
        clip_clipboard[i]->timeline_out -= min_in;
    }

    if (del && copied) {
		delete_selection(sequence->selections, false);
    }
}

void Timeline::relink_clips_using_ids(QVector<int>& old_clips, QVector<Clip*>& new_clips) {
    // relink pasted clips
    for (int i=0;i<old_clips.size();i++) {
        // these indices should correspond
        Clip* oc = sequence->clips.at(old_clips.at(i));
        for (int j=0;j<oc->linked.size();j++) {
            for (int k=0;k<old_clips.size();k++) { // find clip with that ID
                if (oc->linked.at(j) == old_clips.at(k)) {
                    new_clips.at(i)->linked.append(k);
                }
            }
        }
    }
}

void Timeline::paste(bool insert) {
    if (clip_clipboard.size() > 0) {
        ComboAction* ca = new ComboAction();

        // create copies and delete areas that we'll be pasting to
        QVector<Selection> delete_areas;
        QVector<Clip*> pasted_clips;
        long paste_start = LONG_MAX;
        long paste_end = LONG_MIN;
        for (int i=0;i<clip_clipboard.size();i++) {
            Clip* c = clip_clipboard.at(i);

            // create copy of clip and offset by playhead
            Clip* cc = c->copy(sequence);
			cc->timeline_in += sequence->playhead;
			cc->timeline_out += sequence->playhead;
            cc->track = c->track;

            paste_start = qMin(paste_start, cc->timeline_in);
            paste_end = qMax(paste_end, cc->timeline_out);

            pasted_clips.append(cc);

            if (!insert) {
                Selection s;
                s.in = cc->timeline_in;
                s.out = cc->timeline_out;
                s.track = c->track;
                delete_areas.append(s);
            }
        }
        if (insert) {
            split_cache.clear();
            split_all_clips_at_point(ca, sequence->playhead);
            ca->append(new RippleCommand(sequence, paste_start, paste_end - paste_start));
        } else {
            delete_areas_and_relink(ca, delete_areas);
        }

        // correct linked clips
        for (int i=0;i<clip_clipboard.size();i++) {
            // these indices should correspond
            Clip* oc = clip_clipboard.at(i);

            for (int j=0;j<oc->linked.size();j++) {
                for (int k=0;k<clip_clipboard.size();k++) { // find clip with that ID
                    if (clip_clipboard.at(k)->load_id == oc->linked.at(j)) {
                        pasted_clips.at(i)->linked.append(k);
                    }
                }
            }
        }

        ca->append(new AddClipCommand(sequence, pasted_clips));

        undo_stack.push(ca);

		update_ui(true);

        if (config.paste_seeks) {
			panel_sequence_viewer->seek(paste_end);
        }
    }
}

void Timeline::ripple_to_in_point(bool in, bool ripple) {
    if (sequence != NULL) {
        // get track count
        int track_min = 0;
        int track_max = 0;

        bool one_frame_mode = false;

        // find closest in point to playhead
        long in_point = in ? 0 : LONG_MAX;
        for (int i=0;i<sequence->clips.size();i++) {
            Clip* c = sequence->clips.at(i);
			if (c != NULL) {
				track_min = qMin(track_min, c->track);
				track_max = qMax(track_max, c->track);
				if ((in && c->timeline_in > in_point && c->timeline_in <= sequence->playhead)
						|| (!in && c->timeline_in < in_point && c->timeline_in >= sequence->playhead)) {
					in_point = c->timeline_in;
					if (sequence->playhead == in_point) {
						one_frame_mode = true;
						break;
					}
				}
				if ((in && c->timeline_out > in_point && c->timeline_out <= sequence->playhead)
						|| (!in && c->timeline_out < in_point && c->timeline_out > sequence->playhead)) {
					in_point = c->timeline_out;
					if (sequence->playhead == in_point) {
						one_frame_mode = true;
						break;
					}
				}
			}
        }

		QVector<Selection> areas;
		ComboAction* ca = new ComboAction();

        if (one_frame_mode) {
			// set up deletion areas based on track count
			if (in) {
				in_point = sequence->playhead;
			} else {
				in_point = sequence->playhead - 1;
			}

			for (int i=track_min;i<=track_max;i++) {
				Selection s;
				s.in = in_point;
				s.out = in_point + 1;
				s.track = i;
				areas.append(s);
			}

			// trim and move clips around the in point
			delete_areas_and_relink(ca, areas);
			if (ripple) ca->append(new RippleCommand(sequence, in_point, -1));
        } else {
			// set up deletion areas based on track count
            for (int i=track_min;i<=track_max;i++) {
                Selection s;
				s.in = qMin(in_point, sequence->playhead);
				s.out = qMax(in_point, sequence->playhead);
                s.track = i;
                areas.append(s);
			}

			// trim and move clips around the in point
			delete_areas_and_relink(ca, areas);
			if (ripple) ca->append(new RippleCommand(sequence, in_point, (in) ? (in_point - sequence->playhead) : (sequence->playhead - in_point)));
        }

		undo_stack.push(ca);

		update_ui(true);

		if (in_point < sequence->playhead && ripple) panel_sequence_viewer->seek(in_point);
    }
}

bool Timeline::split_selection(ComboAction* ca) {
    bool split = false;

    // temporary relinking vectors
    QVector<int> pre_splits;
    QVector<Clip*> post_splits;
    QVector<Clip*> secondary_post_splits;

    // find clips within selection and split
    for (int j=0;j<sequence->clips.size();j++) {
        Clip* clip = sequence->clips.at(j);
        if (clip != NULL) {
			for (int i=0;i<sequence->selections.size();i++) {
				const Selection& s = sequence->selections.at(i);
                if (s.track == clip->track) {
					if (clip->timeline_in < s.in && clip->timeline_out > s.out) {
                        Clip* split_A = clip->copy(sequence);
                        split_A->clip_in += (s.in - clip->timeline_in);
                        split_A->timeline_in = s.in;
                        split_A->timeline_out = s.out;
                        pre_splits.append(j);
						post_splits.append(split_A);

                        Clip* split_B = clip->copy(sequence);
                        split_B->clip_in += (s.out - clip->timeline_in);
                        split_B->timeline_in = s.out;
                        secondary_post_splits.append(split_B);

                        if (clip->get_opening_transition() != NULL) {
                            split_B->sequence->hard_delete_transition(split_B, TA_OPENING_TRANSITION);
                            split_A->sequence->hard_delete_transition(split_A, TA_OPENING_TRANSITION);
						}

                        if (clip->get_closing_transition() != NULL) {
                            ca->append(new DeleteTransitionCommand(clip, TA_CLOSING_TRANSITION));

                            split_A->sequence->hard_delete_transition(split_A, TA_CLOSING_TRANSITION);
						}

                        ca->append(new MoveClipAction(clip, clip->timeline_in, s.in, clip->clip_in, clip->track));
                        split = true;
                    } else {
                        Clip* post_a = split_clip(ca, j, s.in);
                        Clip* post_b = split_clip(ca, j, s.out);
                        if (post_a != NULL) {
                            pre_splits.append(j);
                            post_splits.append(post_a);
                            split = true;
                        }
                        if (post_b != NULL) {
                            if (post_a != NULL) {
                                pre_splits.append(j);
                                post_splits.append(post_b);
                            } else {
                                secondary_post_splits.append(post_b);
                            }
                            split = true;
                        }
                    }
                }
            }
        }
    }

    if (split) {
        // relink after splitting
        relink_clips_using_ids(pre_splits, post_splits);
        relink_clips_using_ids(pre_splits, secondary_post_splits);
        ca->append(new AddClipCommand(sequence, post_splits));
        ca->append(new AddClipCommand(sequence, secondary_post_splits));

        return true;
    }
    return false;
}

bool Timeline::split_all_clips_at_point(ComboAction* ca, long point) {
    bool split = false;
    for (int j=0;j<sequence->clips.size();j++) {
        Clip* c = sequence->clips.at(j);
        if (c != NULL) {
            // always relinks
            if (split_clip_and_relink(ca, j, point, true)) {
                split = true;
            }
        }
    }
    return split;
}

void Timeline::split_at_playhead() {
    ComboAction* ca = new ComboAction();
    bool split_selected = false;
    split_cache.clear();

	if (sequence->selections.size() > 0) {
        // see if whole clips are selected
        QVector<int> pre_clips;
        QVector<Clip*> post_clips;
        for (int j=0;j<sequence->clips.size();j++) {
            Clip* clip = sequence->clips.at(j);
            if (clip != NULL && is_clip_selected(clip, true)) {
				Clip* s = split_clip(ca, j, sequence->playhead);
                if (s != NULL) {
                    pre_clips.append(j);
                    post_clips.append(s);
                    split_selected = true;
                }
            }
        }

        if (split_selected) {
            // relink clips if we split
            relink_clips_using_ids(pre_clips, post_clips);
            ca->append(new AddClipCommand(sequence, post_clips));
        } else {
            // split a selection if not
            split_selected = split_selection(ca);
        }
    }

    // if nothing was selected or no selections fell within playhead, simply split at playhead
    if (!split_selected) {
        split_selected = split_all_clips_at_point(ca, sequence->playhead);
    }

    if (split_selected) {
        undo_stack.push(ca);
		update_ui(true);
    } else {
        delete ca;
    }
}

void Timeline::deselect_area(long in, long out, int track) {
	int len = sequence->selections.size();
	for (int i=0;i<len;i++) {
		Selection& s = sequence->selections[i];
		if (s.track == track) {
			if (s.in >= in && s.out <= out) {
				// whole selection is in deselect area
				sequence->selections.removeAt(i);
				i--;
				len--;
			} else if (s.in < in && s.out > out) {
				// middle of selection is in deselect area
				Selection new_sel;
				new_sel.in = out;
				new_sel.out = s.out;
				new_sel.track = s.track;
				sequence->selections.append(new_sel);

				s.out = in;
			} else if (s.in < in && s.out > in) {
				// only out point is in deselect area
				s.out = in;
			} else if (s.in < out && s.out > out) {
				// only in point is in deselect area
				s.in = out;
			}
		}
	}
}

bool Timeline::snap_to_point(long point, long* l) {
    int limit = get_snap_range();
    if (*l > point-limit-1 && *l < point+limit+1) {
        snap_point = point;
        *l = point;
        snapped = true;
        return true;
    }
    return false;
}

bool Timeline::snap_to_timeline(long* l, bool use_playhead, bool use_markers, bool use_workarea) {
    snapped = false;
	if (snapping) {
		if (use_playhead && !panel_sequence_viewer->playing) {
			// snap to playhead
			if (snap_to_point(sequence->playhead, l)) return true;
		}

		// snap to marker
		if (use_markers) {
			for (int i=0;i<sequence->markers.size();i++) {
				if (snap_to_point(sequence->markers.at(i).frame, l)) return true;
			}
		}

		// snap to in/out
		if (use_workarea && sequence->using_workarea) {
			if (snap_to_point(sequence->workarea_in, l)) return true;
			if (snap_to_point(sequence->workarea_out, l)) return true;
		}

		// snap to clip/transition
		for (int i=0;i<sequence->clips.size();i++) {
			Clip* c = sequence->clips.at(i);
			if (c != NULL) {
				if (snap_to_point(c->timeline_in, l)) {
					return true;
				} else if (snap_to_point(c->timeline_out, l)) {
					return true;
                } else if (c->get_opening_transition() != NULL
                           && snap_to_point(c->timeline_in + c->get_opening_transition()->length, l)) {
					return true;
                } else if (c->get_closing_transition() != NULL
                           && snap_to_point(c->timeline_out - c->get_closing_transition()->length, l)) {
					return true;
				}
			}
		}
	}
	return false;
}

void Timeline::set_marker() {
	QInputDialog d(this);
	d.setWindowTitle("Set Marker");
	d.setLabelText("Set marker name:");
	d.setInputMode(QInputDialog::TextInput);
	if (d.exec() == QDialog::Accepted) {
		undo_stack.push(new AddMarkerAction(sequence, sequence->playhead, d.textValue()));
	}
}

void Timeline::toggle_links() {
    LinkCommand* command = new LinkCommand();
	command->s = sequence;
    for (int i=0;i<sequence->clips.size();i++) {
        Clip* c = sequence->clips.at(i);
        if (c != NULL && is_clip_selected(c, true)) {
			command->clips.append(i);
            if (c->linked.size() > 0) {
                command->link = false; // prioritize unlinking

				for (int j=0;j<c->linked.size();j++) { // add links to the command
					if (!command->clips.contains(c->linked.at(j))) {
						command->clips.append(c->linked.at(j));
					}
                }
            }
        }
    }
    if (command->clips.size() > 0) {
        undo_stack.push(command);
		repaint_timeline();
    } else {
        delete command;
    }
}

void Timeline::increase_track_height() {
    for (int i=0;i<video_track_heights.size();i++) {
        video_track_heights[i] += TRACK_HEIGHT_INCREMENT;
    }
    for (int i=0;i<audio_track_heights.size();i++) {
        audio_track_heights[i] += TRACK_HEIGHT_INCREMENT;
    }
	repaint_timeline();
}

void Timeline::decrease_track_height() {
    for (int i=0;i<video_track_heights.size();i++) {
        video_track_heights[i] -= TRACK_HEIGHT_INCREMENT;
        if (video_track_heights[i] < TRACK_MIN_HEIGHT) video_track_heights[i] = TRACK_MIN_HEIGHT;
    }
    for (int i=0;i<audio_track_heights.size();i++) {
        audio_track_heights[i] -= TRACK_HEIGHT_INCREMENT;
        if (audio_track_heights[i] < TRACK_MIN_HEIGHT) audio_track_heights[i] = TRACK_MIN_HEIGHT;
    }
	repaint_timeline();
}

void Timeline::deselect() {
	sequence->selections.clear();
	repaint_timeline();
}

long getFrameFromScreenPoint(double zoom, int x) {
	long f = qCeil((float) x / zoom);
	if (f < 0) {
		return 0;
	}
	return f;
}

int getScreenPointFromFrame(double zoom, long frame) {
	return (int) qFloor(frame*zoom);
}

long Timeline::getTimelineFrameFromScreenPoint(int x) {
	return getFrameFromScreenPoint(zoom, x + scroll);
}

int Timeline::getTimelineScreenPointFromFrame(long frame) {
	return getScreenPointFromFrame(zoom, frame) - scroll;
}

void Timeline::on_toolArrowButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_POINTER;
	creating = false;
}

void Timeline::on_toolEditButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::IBeamCursor);
    tool = TIMELINE_TOOL_EDIT;
	creating = false;
}

void Timeline::on_toolRippleButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_RIPPLE;
	creating = false;
}

void Timeline::on_toolRollingButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_ROLLING;
	creating = false;
}

void Timeline::on_toolRazorButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::IBeamCursor);
    tool = TIMELINE_TOOL_RAZOR;
	creating = false;
}

void Timeline::on_toolSlipButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_SLIP;
	creating = false;
}

void Timeline::on_toolSlideButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_SLIDE;
	creating = false;
}

void Timeline::on_addButton_clicked() {
	QMenu add_menu(this);

	QAction* titleMenuItem = new QAction(&add_menu);
	titleMenuItem->setText("Title...");
	titleMenuItem->setData(ADD_OBJ_TITLE);
	add_menu.addAction(titleMenuItem);

	QAction* solidMenuItem = new QAction(&add_menu);
	solidMenuItem->setText("Solid Color...");
	solidMenuItem->setData(ADD_OBJ_SOLID);
	add_menu.addAction(solidMenuItem);

	QAction* barsMenuItem = new QAction(&add_menu);
	barsMenuItem->setText("Bars...");
	barsMenuItem->setData(ADD_OBJ_BARS);
	add_menu.addAction(barsMenuItem);

	add_menu.addSeparator();

	QAction* toneMenuItem = new QAction(&add_menu);
	toneMenuItem->setText("Tone...");
	toneMenuItem->setData(ADD_OBJ_TONE);
	add_menu.addAction(toneMenuItem);

	QAction* noiseMenuItem = new QAction(&add_menu);
	noiseMenuItem->setText("Noise...");
	noiseMenuItem->setData(ADD_OBJ_NOISE);
	add_menu.addAction(noiseMenuItem);

	connect(&add_menu, SIGNAL(triggered(QAction*)), this, SLOT(addMenuItem(QAction*)));

	add_menu.exec(QCursor::pos());
}

void Timeline::addMenuItem(QAction* action) {
	creating = true;
	creating_object = action->data().toInt();
}

void Timeline::setScroll(int s) {
	scroll = s;
	ui->headers->set_scroll(s);
	repaint_timeline();
}

void Timeline::on_recordButton_clicked() {
	if (project_url.isEmpty()) {
		QMessageBox::critical(this, "Unsaved Project", "You must save this project before you can record audio in it.", QMessageBox::Ok);
	} else {
		creating = true;
		creating_object = ADD_OBJ_AUDIO;
	}
}

void Timeline::on_toolTransitionButton_clicked() {
	creating = false;

	QMenu transition_menu(this);

	for (int i=0;i<video_effects.size();i++) {
		const EffectMeta& em = video_effects.at(i);
		if (em.type == EFFECT_TYPE_TRANSITION) {
			QAction* a = transition_menu.addAction(em.name);
			a->setObjectName("v");
			a->setData(reinterpret_cast<quintptr>(&em));
		}
	}

	transition_menu.addSeparator();

	for (int i=0;i<audio_effects.size();i++) {
		const EffectMeta& em = audio_effects.at(i);
		if (em.type == EFFECT_TYPE_TRANSITION) {
			QAction* a = transition_menu.addAction(em.name);
			a->setObjectName("a");
			a->setData(reinterpret_cast<quintptr>(&em));
		}
	}

	connect(&transition_menu, SIGNAL(triggered(QAction*)), this, SLOT(transition_menu_select(QAction*)));

    ui->toolTransitionButton->setChecked(false);

	transition_menu.exec(QCursor::pos());
}

void Timeline::transition_menu_select(QAction* a) {
	transition_tool_meta = reinterpret_cast<const EffectMeta*>(a->data().value<quintptr>());

	if (a->objectName() == "v") {
		transition_tool_side = -1;
	} else {
		transition_tool_side = 1;
	}

	decheck_tool_buttons(sender());
	ui->timeline_area->setCursor(Qt::CrossCursor);
	tool = TIMELINE_TOOL_TRANSITION;
    ui->toolTransitionButton->setChecked(true);
}
