/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "notationinteraction.h"

#include "log.h"

#include <memory>
#include <QRectF>
#include <QPainter>
#include <QClipboard>
#include <QApplication>

#include "defer.h"
#include "ptrutils.h"

#include "engraving/rw/xml.h"
#include "engraving/infrastructure/draw/pen.h"
#include "engraving/infrastructure/draw/painterpath.h"

#include "libmscore/masterscore.h"
#include "libmscore/page.h"
#include "libmscore/shadownote.h"
#include "libmscore/staff.h"
#include "libmscore/measure.h"
#include "libmscore/part.h"
#include "libmscore/drumset.h"
#include "libmscore/rest.h"
#include "libmscore/slur.h"
#include "libmscore/system.h"
#include "libmscore/chord.h"
#include "libmscore/elementgroup.h"
#include "libmscore/textframe.h"
#include "libmscore/figuredbass.h"
#include "libmscore/stafflines.h"
#include "libmscore/stafftype.h"
#include "libmscore/actionicon.h"
#include "libmscore/undo.h"
#include "libmscore/navigate.h"
#include "libmscore/keysig.h"
#include "libmscore/instrchange.h"
#include "libmscore/lasso.h"
#include "libmscore/textedit.h"
#include "libmscore/lyrics.h"
#include "libmscore/bracket.h"
#include "libmscore/factory.h"
#include "libmscore/image.h"
#include "libmscore/mscore.h"
#include "libmscore/linkedobjects.h"
#include "libmscore/tuplet.h"

#include "masternotation.h"
#include "scorecallbacks.h"
#include "notationnoteinput.h"
#include "notationselection.h"

using namespace mu::notation;
using namespace mu::framework;
using namespace mu::engraving;

static int findSystemIndex(Ms::EngravingItem* element)
{
    Ms::System* target = element->parent()->isSystem() ? toSystem(element->parent()) : element->findMeasureBase()->system();
    return element->score()->systems().indexOf(target);
}

static int findBracketIndex(Ms::EngravingItem* element)
{
    if (!element->isBracket()) {
        return -1;
    }
    Ms::Bracket* bracket = toBracket(element);
    return bracket->system()->brackets().indexOf(bracket);
}

NotationInteraction::NotationInteraction(Notation* notation, INotationUndoStackPtr undoStack)
    : m_notation(notation), m_undoStack(undoStack), m_editData(&m_scoreCallbacks)
{
    m_noteInput = std::make_shared<NotationNoteInput>(notation, this, m_undoStack);
    m_selection = std::make_shared<NotationSelection>(notation);

    m_noteInput->stateChanged().onNotify(this, [this]() {
        if (!m_noteInput->isNoteInputMode()) {
            hideShadowNote();
        }
    });

    m_dragData.ed = Ms::EditData(&m_scoreCallbacks);
    m_dropData.ed = Ms::EditData(&m_scoreCallbacks);
    m_scoreCallbacks.setScore(notation->score());
}

NotationInteraction::~NotationInteraction()
{
    delete m_shadowNote;
}

void NotationInteraction::init()
{
    m_shadowNote = new Ms::ShadowNote(score());
    m_shadowNote->setVisible(false);
}

Ms::Score* NotationInteraction::score() const
{
    return m_notation->score();
}

void NotationInteraction::startEdit()
{
    m_notifyAboutDropChanged = false;
    m_undoStack->prepareChanges();
    m_selectionState = score()->selection().state();
}

void NotationInteraction::apply()
{
    m_undoStack->commitChanges();
    if (m_notifyAboutDropChanged) {
        notifyAboutDropChanged();
    } else {
        notifyAboutNotationChanged();
    }
    if (m_selectionState != score()->selection().state()) {
        notifyAboutSelectionChanged();
    }
}

void NotationInteraction::rollback()
{
    m_undoStack->rollbackChanges();
}

void NotationInteraction::notifyAboutDragChanged()
{
    m_dragChanged.notify();
}

void NotationInteraction::notifyAboutDropChanged()
{
    m_dropChanged.notify();
}

void NotationInteraction::notifyAboutNotationChanged()
{
    m_notation->notifyAboutNotationChanged();
}

void NotationInteraction::notifyAboutTextEditingStarted()
{
    m_textEditingStarted.notify();
}

void NotationInteraction::notifyAboutTextEditingChanged()
{
    m_textEditingChanged.notify();
}

void NotationInteraction::notifyAboutSelectionChanged()
{
    updateGripEdit();
    m_selectionChanged.notify();
}

void NotationInteraction::paint(mu::draw::Painter* painter)
{
    m_shadowNote->draw(painter);

    drawAnchorLines(painter);
    drawTextEditMode(painter);
    drawSelectionRange(painter);
    drawGripPoints(painter);
    if (m_lasso && !m_lasso->isEmpty()) {
        m_lasso->draw(painter);
    }
}

INotationNoteInputPtr NotationInteraction::noteInput() const
{
    return m_noteInput;
}

void NotationInteraction::showShadowNote(const PointF& pos)
{
    const Ms::InputState& inputState = score()->inputState();
    Ms::Position position;
    if (!score()->getPosition(&position, pos, inputState.voice())) {
        m_shadowNote->setVisible(false);
        return;
    }

    Staff* staff = score()->staff(position.staffIdx);
    const Ms::Instrument* instr = staff->part()->instrument();

    Ms::Segment* segment = position.segment;
    qreal segmentSkylineTopY = 0;
    qreal segmentSkylineBottomY = 0;

    Ms::Segment* shadowNoteActualSegment = position.segment->prev1enabled();
    if (shadowNoteActualSegment) {
        segment = shadowNoteActualSegment;
        segmentSkylineTopY = shadowNoteActualSegment->elementsTopOffsetFromSkyline(position.staffIdx);
        segmentSkylineBottomY = shadowNoteActualSegment->elementsBottomOffsetFromSkyline(position.staffIdx);
    }

    Fraction tick = segment->tick();
    qreal mag = staff->staffMag(tick);

    // in any empty measure, pos will be right next to barline
    // so pad this by barNoteDistance
    qreal relX = position.pos.x() - position.segment->measure()->canvasPos().x();
    position.pos.rx() -= qMin(relX - score()->styleMM(Ms::Sid::barNoteDistance) * mag, 0.0);

    Ms::NoteHeadGroup noteheadGroup = Ms::NoteHeadGroup::HEAD_NORMAL;
    Ms::NoteHeadType noteHead = inputState.duration().headType();
    int line = position.line;

    if (instr->useDrumset()) {
        const Ms::Drumset* ds  = instr->drumset();
        int pitch = inputState.drumNote();
        if (pitch >= 0 && ds->isValid(pitch)) {
            line = ds->line(pitch);
            noteheadGroup = ds->noteHead(pitch);
        }
    }

    int voice = 0;
    if (inputState.drumNote() != -1 && inputState.drumset() && inputState.drumset()->isValid(inputState.drumNote())) {
        voice = inputState.drumset()->voice(inputState.drumNote());
    } else {
        voice = inputState.voice();
    }

    m_shadowNote->setVisible(true);
    m_shadowNote->setMag(mag);
    m_shadowNote->setTick(tick);
    m_shadowNote->setStaffIdx(position.staffIdx);
    m_shadowNote->setVoice(voice);
    m_shadowNote->setLineIndex(line);

    Ms::SymId symNotehead;
    Ms::TDuration duration(inputState.duration());

    if (inputState.rest()) {
        int yo = 0;
        Ms::Rest* rest = mu::engraving::Factory::createRest(Ms::gpaletteScore->dummy()->segment(), duration.type());
        rest->setTicks(duration.fraction());
        symNotehead = rest->getSymbol(inputState.duration().type(), 0, staff->lines(position.segment->tick()), &yo);
        m_shadowNote->setState(symNotehead, duration, true, segmentSkylineTopY, segmentSkylineBottomY);
        delete rest;
    } else {
        if (Ms::NoteHeadGroup::HEAD_CUSTOM == noteheadGroup) {
            symNotehead = instr->drumset()->noteHeads(inputState.drumNote(), noteHead);
        } else {
            symNotehead = Note::noteHead(0, noteheadGroup, noteHead);
        }

        m_shadowNote->setState(symNotehead, duration, false, segmentSkylineTopY, segmentSkylineBottomY);
    }

    m_shadowNote->layout();
    m_shadowNote->setPos(position.pos);
}

void NotationInteraction::hideShadowNote()
{
    m_shadowNote->setVisible(false);
}

void NotationInteraction::toggleVisible()
{
    startEdit();

    // TODO: Update `score()->cmdToggleVisible()` and call that here?
    for (EngravingItem* el : selection()->elements()) {
        if (el->isBracket()) {
            continue;
        }
        el->undoChangeProperty(Ms::Pid::VISIBLE, !el->visible());
    }

    apply();

    notifyAboutNotationChanged();
}

EngravingItem* NotationInteraction::hitElement(const PointF& pos, float width) const
{
    QList<Ms::EngravingItem*> elements = hitElements(pos, width);
    if (elements.isEmpty()) {
        return nullptr;
    }
    m_selection->onElementHit(elements.first());
    return elements.first();
}

Staff* NotationInteraction::hitStaff(const PointF& pos) const
{
    return hitMeasure(pos).staff;
}

Ms::Page* NotationInteraction::point2page(const PointF& p) const
{
    if (score()->linearMode()) {
        return score()->pages().isEmpty() ? 0 : score()->pages().front();
    }
    foreach (Ms::Page* page, score()->pages()) {
        if (page->bbox().translated(page->pos()).contains(p)) {
            return page;
        }
    }
    return nullptr;
}

QList<EngravingItem*> NotationInteraction::elementsAt(const PointF& p) const
{
    QList<EngravingItem*> el;
    Ms::Page* page = point2page(p);
    if (page) {
        el = page->items(p - page->pos());
        std::sort(el.begin(), el.end(), NotationInteraction::elementIsLess);
    }
    return el;
}

EngravingItem* NotationInteraction::elementAt(const PointF& p) const
{
    QList<EngravingItem*> el = elementsAt(p);
    EngravingItem* e = el.value(0);
    if (e && e->isPage()) {
        e = el.value(1);
    }
    return e;
}

QList<Ms::EngravingItem*> NotationInteraction::hitElements(const PointF& p_in, float w) const
{
    Ms::Page* page = point2page(p_in);
    if (!page) {
        return QList<Ms::EngravingItem*>();
    }

    QList<Ms::EngravingItem*> ll;

    PointF p = p_in - page->pos();

    if (isTextEditingStarted()) {
        auto editW = w * 2;
        RectF hitRect(p.x() - editW, p.y() - editW, 2.0 * editW, 2.0 * editW);
        if (m_editData.element->intersects(hitRect)) {
            ll.push_back(m_editData.element);
            return ll;
        }
    }

    RectF r(p.x() - w, p.y() - w, 3.0 * w, 3.0 * w);

    QList<Ms::EngravingItem*> elements = page->items(r);
    //! TODO
    //    for (int i = 0; i < MAX_HEADERS; i++)
    //        if (score()->headerText(i) != nullptr)      // gives the ability to select the header
    //            el.push_back(score()->headerText(i));
    //    for (int i = 0; i < MAX_FOOTERS; i++)
    //        if (score()->footerText(i) != nullptr)      // gives the ability to select the footer
    //            el.push_back(score()->footerText(i));
    //! -------

    for (Ms::EngravingItem* element : elements) {
        element->itemDiscovered = 0;
        if (!element->selectable() || element->isPage()) {
            continue;
        }

        if (!element->isInteractionAvailable()) {
            continue;
        }

        if (element->contains(p)) {
            ll.append(element);
        }
    }

    int n = ll.size();
    if ((n == 0) || ((n == 1) && (ll[0]->isMeasure()))) {
        //
        // if no relevant element hit, look nearby
        //
        for (Ms::EngravingItem* element : elements) {
            if (element->isPage() || !element->selectable()) {
                continue;
            }

            if (!element->isInteractionAvailable()) {
                continue;
            }

            if (element->intersects(r)) {
                ll.append(element);
            }
        }
    }

    if (!ll.empty()) {
        std::sort(ll.begin(), ll.end(), NotationInteraction::elementIsLess);
    } else {
        Ms::Measure* measure = hitMeasure(p_in).measure;
        if (measure) {
            ll << measure;
        }
    }

    return ll;
}

NotationInteraction::HitMeasureData NotationInteraction::hitMeasure(const PointF& pos) const
{
    int staffIndex = -1;
    Ms::Segment* segment = nullptr;
    PointF offset;
    Measure* measure = score()->pos2measure(pos, &staffIndex, 0, &segment, &offset);

    HitMeasureData result;
    if (measure && measure->staffLines(staffIndex)->canvasBoundingRect().contains(pos)) {
        result.measure = measure;
        result.staff = score()->staff(staffIndex);
    }

    return result;
}

bool NotationInteraction::elementIsLess(const Ms::EngravingItem* e1, const Ms::EngravingItem* e2)
{
    if (!e1->selectable()) {
        return false;
    }
    if (!e2->selectable()) {
        return true;
    }
    if (e1->isNote() && e2->isStem()) {
        return true;
    }
    if (e2->isNote() && e1->isStem()) {
        return false;
    }
    if (e1->z() == e2->z()) {
        // same stacking order, prefer non-hidden elements
        if (e1->type() == e2->type()) {
            if (e1->type() == Ms::ElementType::NOTEDOT) {
                const Ms::NoteDot* n1 = static_cast<const Ms::NoteDot*>(e1);
                const Ms::NoteDot* n2 = static_cast<const Ms::NoteDot*>(e2);
                if (n1->note() && n1->note()->hidden()) {
                    return false;
                } else if (n2->note() && n2->note()->hidden()) {
                    return true;
                }
            } else if (e1->type() == Ms::ElementType::NOTE) {
                const Ms::Note* n1 = static_cast<const Ms::Note*>(e1);
                const Ms::Note* n2 = static_cast<const Ms::Note*>(e2);
                if (n1->hidden()) {
                    return false;
                } else if (n2->hidden()) {
                    return true;
                }
            }
        }
        // different types, or same type but nothing hidden - use track
        return e1->track() < e2->track();
    }

    // default case, use stacking order
    return e1->z() < e2->z();
}

const NotationInteraction::HitElementContext& NotationInteraction::hitElementContext() const
{
    return m_hitElementContext;
}

void NotationInteraction::setHitElementContext(const HitElementContext& context)
{
    m_hitElementContext = context;
}

void NotationInteraction::moveChordNoteSelection(MoveDirection d)
{
    IF_ASSERT_FAILED(MoveDirection::Up == d || MoveDirection::Down == d) {
        return;
    }

    EngravingItem* current = selection()->element();
    if (!current || !(current->isNote() || current->isRest())) {
        return;
    }

    EngravingItem* chordElem;
    if (d == MoveDirection::Up) {
        chordElem = score()->upAlt(current);
    } else {
        chordElem = score()->downAlt(current);
    }

    if (chordElem == current) {
        return;
    }

    select({ chordElem }, SelectType::SINGLE, chordElem->staffIdx());
}

void NotationInteraction::moveSegmentSelection(MoveDirection d)
{
    IF_ASSERT_FAILED(MoveDirection::Left == d || MoveDirection::Right == d) {
        return;
    }
    EngravingItem* e = selection()->element();
    if (!e && !selection()->elements().empty()) {
        e = d == MoveDirection::Left ? selection()->elements().front() : selection()->elements().back();
    }
    if (!e || (e = d == MoveDirection::Left ? e->prevSegmentElement() : e->nextSegmentElement()) == nullptr) {
        e = d == MoveDirection::Left ? score()->firstElement() : score()->lastElement();
    }
    select({ e }, SelectType::SINGLE);
}

void NotationInteraction::selectTopOrBottomOfChord(MoveDirection d)
{
    IF_ASSERT_FAILED(MoveDirection::Up == d || MoveDirection::Down == d) {
        return;
    }

    EngravingItem* current = selection()->element();
    if (!current || !current->isNote()) {
        return;
    }

    EngravingItem* target = d == MoveDirection::Up
                            ? score()->upAltCtrl(toNote(current)) : score()->downAltCtrl(toNote(current));

    if (target == current) {
        return;
    }

    select({ target }, SelectType::SINGLE);
}

void NotationInteraction::doSelect(const std::vector<EngravingItem*>& elements, SelectType type, int staffIndex)
{
    TRACEFUNC;

    if (needEndTextEditing(elements)) {
        endEditText();
    } else if (isElementEditStarted() && (elements.size() != 1 || elements.front() != score()->selection().element())) {
        endEditElement();
    }
    if (elements.size() == 1 && type == SelectType::ADD && QGuiApplication::keyboardModifiers() == Qt::KeyboardModifier::ControlModifier) {
        if (score()->selection().isRange()) {
            score()->selection().setState(Ms::SelState::LIST);
            score()->setUpdateAll();
        }
        if (elements.front()->selected()) {
            score()->deselect(elements.front());
            return;
        }
    }

    if (type == SelectType::REPLACE) {
        score()->deselectAll();
        type = SelectType::ADD;
    }

    for (EngravingItem* element: elements) {
        score()->select(element, type, staffIndex);
    }
}

void NotationInteraction::select(const std::vector<EngravingItem*>& elements, SelectType type, int staffIndex)
{
    TRACEFUNC;

    doSelect(elements, type, staffIndex);
    notifyAboutSelectionChanged();
}

void NotationInteraction::selectAll()
{
    if (isTextEditingStarted()) {
        auto textBase = toTextBase(m_editData.element);
        textBase->selectAll(textBase->cursorFromEditData(m_editData));
    } else {
        score()->cmdSelectAll();
    }

    notifyAboutSelectionChanged();
}

void NotationInteraction::selectSection()
{
    score()->cmdSelectSection();

    notifyAboutSelectionChanged();
}

void NotationInteraction::selectFirstElement(bool frame)
{
    EngravingItem* element = score()->firstElement(frame);
    select({ element }, SelectType::SINGLE, element->staffIdx());
}

void NotationInteraction::selectLastElement()
{
    EngravingItem* element = score()->lastElement();
    select({ element }, SelectType::SINGLE, element->staffIdx());
}

INotationSelectionPtr NotationInteraction::selection() const
{
    return m_selection;
}

void NotationInteraction::clearSelection()
{
    if (isElementEditStarted()) {
        endEditElement();
    } else if (m_editData.element) {
        m_editData.element = nullptr;
    }
    if (isDragStarted()) {
        doEndDrag();
        rollback();
        notifyAboutNotationChanged();
    }
    if (selection()->isNone()) {
        return;
    }

    score()->deselectAll();

    notifyAboutSelectionChanged();

    setHitElementContext(HitElementContext());
}

mu::async::Notification NotationInteraction::selectionChanged() const
{
    return m_selectionChanged;
}

bool NotationInteraction::isSelectionTypeFiltered(SelectionFilterType type) const
{
    return score()->selectionFilter().isFiltered(type);
}

void NotationInteraction::setSelectionTypeFiltered(SelectionFilterType type, bool filtered)
{
    score()->selectionFilter().setFiltered(type, filtered);
    if (selection()->isRange()) {
        score()->selection().updateSelectedElements();
        notifyAboutSelectionChanged();
    }
}

bool NotationInteraction::isDragStarted() const
{
    if (m_dragData.dragGroups.size() > 0) {
        return true;
    }

    if (m_lasso && !m_lasso->isEmpty()) {
        return true;
    }

    return false;
}

void NotationInteraction::DragData::reset()
{
    beginMove = QPointF();
    elementOffset = QPointF();
    ed = Ms::EditData(ed.view());
    dragGroups.clear();
}

void NotationInteraction::startDrag(const std::vector<EngravingItem*>& elems,
                                    const PointF& eoffset,
                                    const IsDraggable& isDraggable)
{
    m_dragData.reset();
    m_dragData.elements = elems;
    m_dragData.elementOffset = eoffset;
    m_editData.modifiers = QGuiApplication::keyboardModifiers();

    for (EngravingItem* e : m_dragData.elements) {
        if (!isDraggable(e)) {
            continue;
        }

        std::unique_ptr<Ms::ElementGroup> g = e->getDragGroup(isDraggable);
        if (g && g->enabled()) {
            m_dragData.dragGroups.push_back(std::move(g));
        }
    }

    startEdit();

    qreal zoom = configuration()->currentZoom().val / 100.f;
    qreal proximity = configuration()->selectionProximity() * 0.5f / zoom;
    m_scoreCallbacks.setScore(score());
    m_scoreCallbacks.setSelectionProximity(proximity);

    if (isGripEditStarted()) {
        m_editData.element->startEditDrag(m_editData);
        return;
    }

    m_dragData.ed.modifiers = QGuiApplication::keyboardModifiers();
    for (auto& group : m_dragData.dragGroups) {
        group->startDrag(m_dragData.ed);
    }
}

void NotationInteraction::doDragLasso(const PointF& pt)
{
    if (!m_lasso) {
        m_lasso = new Ms::Lasso(score());
    }

    score()->addRefresh(m_lasso->canvasBoundingRect());
    RectF r;
    r.setCoords(m_dragData.beginMove.x(), m_dragData.beginMove.y(), pt.x(), pt.y());
    m_lasso->setbbox(r.normalized());
    score()->addRefresh(m_lasso->canvasBoundingRect());
    score()->lassoSelect(m_lasso->bbox());
    score()->update();
}

void NotationInteraction::endLasso()
{
    if (!m_lasso) {
        return;
    }

    score()->addRefresh(m_lasso->canvasBoundingRect());
    m_lasso->setbbox(RectF());
    score()->lassoSelectEnd(m_dragData.mode != DragMode::LassoList);
    score()->update();
}

void NotationInteraction::drag(const PointF& fromPos, const PointF& toPos, DragMode mode)
{
    if (m_dragData.beginMove.isNull()) {
        m_dragData.beginMove = fromPos;
        m_dragData.ed.pos = fromPos;
    }
    m_dragData.mode = mode;

    PointF normalizedBegin = m_dragData.beginMove - m_dragData.elementOffset;

    PointF delta = toPos - normalizedBegin;
    PointF evtDelta = toPos - m_dragData.ed.pos;

    switch (mode) {
    case DragMode::BothXY:
    case DragMode::LassoList:
        break;
    case DragMode::OnlyX:
        delta.setY(m_dragData.ed.delta.y());
        evtDelta.setY(0.0);
        break;
    case DragMode::OnlyY:
        delta.setX(m_dragData.ed.delta.x());
        evtDelta.setX(0.0);
        break;
    }

    m_dragData.ed.lastPos = m_dragData.ed.pos;

    m_dragData.ed.hRaster = configuration()->isSnappedToGrid(framework::Orientation::Horizontal);
    m_dragData.ed.vRaster = configuration()->isSnappedToGrid(framework::Orientation::Vertical);
    m_dragData.ed.delta = delta;
    m_dragData.ed.moveDelta = delta - m_dragData.elementOffset;
    m_dragData.ed.evtDelta = evtDelta;
    m_dragData.ed.pos = toPos;
    m_dragData.ed.modifiers = QGuiApplication::keyboardModifiers();

    if (isTextEditingStarted()) {
        m_editData.pos = toPos;
        toTextBase(m_editData.element)->dragTo(m_editData);

        notifyAboutTextEditingChanged();
        return;
    }

    if (isGripEditStarted()) {
        m_dragData.ed.curGrip = m_editData.curGrip;
        m_dragData.ed.delta = m_dragData.ed.pos - m_dragData.ed.lastPos;
        m_dragData.ed.moveDelta = m_dragData.ed.delta - m_dragData.elementOffset;
        m_dragData.ed.addData(m_editData.getData(m_editData.element));
        m_editData.element->editDrag(m_dragData.ed);
    } else if (isElementEditStarted()) {
        m_dragData.ed.delta = evtDelta;
        m_editData.element->editDrag(m_dragData.ed);
    } else {
        for (auto& group : m_dragData.dragGroups) {
            score()->addRefresh(group->drag(m_dragData.ed));
        }
    }

    score()->update();

    if (isGripEditStarted()) {
        updateAnchorLines();
    } else {
        std::vector<LineF> anchorLines;
        for (const EngravingItem* e : selection()->elements()) {
            QVector<LineF> elAnchorLines = e->dragAnchorLines();
            if (!elAnchorLines.isEmpty()) {
                for (LineF& l : elAnchorLines) {
                    anchorLines.push_back(l);
                }
            }
        }
        setAnchorLines(anchorLines);
    }
    if (m_dragData.elements.size() == 0) {
        doDragLasso(toPos);
    }

    notifyAboutDragChanged();
}

void NotationInteraction::doEndDrag()
{
    if (isGripEditStarted()) {
        m_editData.element->endEditDrag(m_editData);
        m_editData.element->endEdit(m_editData);
    } else {
        for (auto& group : m_dragData.dragGroups) {
            group->endDrag(m_dragData.ed);
        }
        if (m_lasso && !m_lasso->isEmpty()) {
            endLasso();
        }
    }

    m_dragData.reset();
    resetAnchorLines();
}

void NotationInteraction::endDrag()
{
    doEndDrag();
    apply();
    notifyAboutDragChanged();

    //    updateGrips();
    //    if (editData.element->normalModeEditBehavior() == EngravingItem::EditBehavior::Edit
    //        && score()->selection().element() == editData.element) {
    //        startEdit(/* editMode */ false);
    //    }
}

mu::async::Notification NotationInteraction::dragChanged() const
{
    return m_dragChanged;
}

//! NOTE Copied from ScoreView::dragEnterEvent
void NotationInteraction::startDrop(const QByteArray& edata)
{
    resetDropElement();

    Ms::XmlReader e(edata);
    m_dropData.ed.dragOffset = QPointF();
    Fraction duration;      // dummy
    ElementType type = EngravingItem::readType(e, &m_dropData.ed.dragOffset, &duration);

    EngravingItem* el = engraving::Factory::createItem(type, score()->dummy());
    if (el) {
        if (type == ElementType::BAR_LINE || type == ElementType::ARPEGGIO || type == ElementType::BRACKET) {
            double spatium = score()->spatium();
            el->setHeight(spatium * 5);
        }
        m_dropData.ed.dropElement = el;
        m_dropData.ed.dropElement->setParent(0);
        m_dropData.ed.dropElement->read(e);
        m_dropData.ed.dropElement->layout();
    }
}

bool NotationInteraction::startDrop(const QUrl& url)
{
    if (url.scheme() != "file") {
        return false;
    }

    auto image = static_cast<Ms::Image*>(Factory::createItem(Ms::ElementType::IMAGE, score()->dummy()));
    if (!image->load(url.toLocalFile())) {
        return false;
    }

    resetDropElement();

    m_dropData.ed.dropElement = image;
    m_dropData.ed.dragOffset = QPointF();
    m_dropData.ed.dropElement->setParent(nullptr);
    m_dropData.ed.dropElement->layout();

    return true;
}

//! NOTE Copied from ScoreView::dragMoveEvent
bool NotationInteraction::isDropAccepted(const PointF& pos, Qt::KeyboardModifiers modifiers)
{
    if (!m_dropData.ed.dropElement) {
        return false;
    }

    m_dropData.ed.pos = pos;
    m_dropData.ed.modifiers = modifiers;

    switch (m_dropData.ed.dropElement->type()) {
    case ElementType::VOLTA:
        return dragMeasureAnchorElement(pos);
    case ElementType::PEDAL:
    case ElementType::LET_RING:
    case ElementType::VIBRATO:
    case ElementType::PALM_MUTE:
    case ElementType::OTTAVA:
    case ElementType::TRILL:
    case ElementType::HAIRPIN:
    case ElementType::TEXTLINE:
        return dragTimeAnchorElement(pos);
    case ElementType::IMAGE:
    case ElementType::SYMBOL:
    case ElementType::FSYMBOL:
    case ElementType::DYNAMIC:
    case ElementType::KEYSIG:
    case ElementType::CLEF:
    case ElementType::TIMESIG:
    case ElementType::BAR_LINE:
    case ElementType::ARPEGGIO:
    case ElementType::BREATH:
    case ElementType::GLISSANDO:
    case ElementType::MEASURE_NUMBER:
    case ElementType::BRACKET:
    case ElementType::ARTICULATION:
    case ElementType::FERMATA:
    case ElementType::CHORDLINE:
    case ElementType::BEND:
    case ElementType::ACCIDENTAL:
    case ElementType::TEXT:
    case ElementType::FINGERING:
    case ElementType::TEMPO_TEXT:
    case ElementType::STAFF_TEXT:
    case ElementType::SYSTEM_TEXT:
    case ElementType::NOTEHEAD:
    case ElementType::TREMOLO:
    case ElementType::LAYOUT_BREAK:
    case ElementType::MARKER:
    case ElementType::STAFF_STATE:
    case ElementType::INSTRUMENT_CHANGE:
    case ElementType::REHEARSAL_MARK:
    case ElementType::JUMP:
    case ElementType::MEASURE_REPEAT:
    case ElementType::ACTION_ICON:
    case ElementType::CHORD:
    case ElementType::SPACER:
    case ElementType::SLUR:
    case ElementType::HARMONY:
    case ElementType::BAGPIPE_EMBELLISHMENT:
    case ElementType::AMBITUS:
    case ElementType::TREMOLOBAR:
    case ElementType::FIGURED_BASS:
    case ElementType::LYRICS:
    case ElementType::FRET_DIAGRAM:
    case ElementType::STAFFTYPE_CHANGE: {
        EngravingItem* e = dropTarget(m_dropData.ed);
        if (e) {
            if (!e->isMeasure()) {
                setDropTarget(e);
            }
            return true;
        } else {
            return false;
        }
    }
    break;
    default:
        break;
    }

    return false;
}

//! NOTE Copied from ScoreView::dropEvent
bool NotationInteraction::drop(const PointF& pos, Qt::KeyboardModifiers modifiers)
{
    if (!m_dropData.ed.dropElement) {
        return false;
    }

    IF_ASSERT_FAILED(m_dropData.ed.dropElement->score() == score()) {
        return false;
    }

    bool accepted = false;
    bool needNotifyAboutSelectionChanged = false;

    m_dropData.ed.pos       = pos;
    m_dropData.ed.modifiers = modifiers;
    m_dropData.ed.dropElement->styleChanged();

    bool firstStaffOnly = false;
    bool applyUserOffset = false;

    startEdit();
    score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());

    switch (m_dropData.ed.dropElement->type()) {
    case ElementType::TEXTLINE:
        firstStaffOnly = m_dropData.ed.dropElement->systemFlag();
    // fall-thru
    case ElementType::VOLTA:
        // voltas drop to first staff by default, or closest staff if Control is held
        firstStaffOnly = firstStaffOnly || !(m_dropData.ed.modifiers & Qt::ControlModifier);
    // fall-thru
    case ElementType::OTTAVA:
    case ElementType::TRILL:
    case ElementType::PEDAL:
    case ElementType::LET_RING:
    case ElementType::VIBRATO:
    case ElementType::PALM_MUTE:
    case ElementType::HAIRPIN:
    {
        Ms::Spanner* spanner = ptr::checked_cast<Ms::Spanner>(m_dropData.ed.dropElement);
        score()->cmdAddSpanner(spanner, pos, firstStaffOnly);
        score()->setUpdateAll();
        accepted = true;
    }
    break;
    case ElementType::SYMBOL:
    case ElementType::FSYMBOL:
    case ElementType::IMAGE:
        applyUserOffset = true;
    // fall-thru
    case ElementType::DYNAMIC:
    case ElementType::FRET_DIAGRAM:
    case ElementType::HARMONY:
    {
        EngravingItem* el = elementAt(pos);
        if (el == 0 || el->type() == ElementType::STAFF_LINES) {
            int staffIdx;
            Ms::Segment* seg;
            PointF offset;
            el = score()->pos2measure(pos, &staffIdx, 0, &seg, &offset);
            if (el && el->isMeasure()) {
                m_dropData.ed.dropElement->setTrack(staffIdx * Ms::VOICES);
                m_dropData.ed.dropElement->setParent(seg);

                if (applyUserOffset) {
                    m_dropData.ed.dropElement->setOffset(offset);
                }
                score()->undoAddElement(m_dropData.ed.dropElement);
            } else {
                qDebug("cannot drop here");
                resetDropElement();
            }
        } else {
            score()->addRefresh(el->canvasBoundingRect());
            score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());

            if (!el->acceptDrop(m_dropData.ed)) {
                qDebug("drop %s onto %s not accepted", m_dropData.ed.dropElement->name(), el->name());
                break;
            }
            EngravingItem* dropElement = el->drop(m_dropData.ed);
            score()->addRefresh(el->canvasBoundingRect());
            if (dropElement) {
                doSelect({ dropElement }, SelectType::SINGLE);
                needNotifyAboutSelectionChanged = true;
                score()->addRefresh(dropElement->canvasBoundingRect());
            }
        }
    }
        accepted = true;
        break;
    case ElementType::HBOX:
    case ElementType::VBOX:
    case ElementType::KEYSIG:
    case ElementType::CLEF:
    case ElementType::TIMESIG:
    case ElementType::BAR_LINE:
    case ElementType::ARPEGGIO:
    case ElementType::BREATH:
    case ElementType::GLISSANDO:
    case ElementType::MEASURE_NUMBER:
    case ElementType::BRACKET:
    case ElementType::ARTICULATION:
    case ElementType::FERMATA:
    case ElementType::CHORDLINE:
    case ElementType::BEND:
    case ElementType::ACCIDENTAL:
    case ElementType::TEXT:
    case ElementType::FINGERING:
    case ElementType::TEMPO_TEXT:
    case ElementType::STAFF_TEXT:
    case ElementType::SYSTEM_TEXT:
    case ElementType::NOTEHEAD:
    case ElementType::TREMOLO:
    case ElementType::LAYOUT_BREAK:
    case ElementType::MARKER:
    case ElementType::STAFF_STATE:
    case ElementType::INSTRUMENT_CHANGE:
    case ElementType::REHEARSAL_MARK:
    case ElementType::JUMP:
    case ElementType::MEASURE_REPEAT:
    case ElementType::ACTION_ICON:
    case ElementType::NOTE:
    case ElementType::CHORD:
    case ElementType::SPACER:
    case ElementType::SLUR:
    case ElementType::BAGPIPE_EMBELLISHMENT:
    case ElementType::AMBITUS:
    case ElementType::TREMOLOBAR:
    case ElementType::FIGURED_BASS:
    case ElementType::LYRICS:
    case ElementType::STAFFTYPE_CHANGE: {
        EngravingItem* el = dropTarget(m_dropData.ed);
        if (!el) {
            if (!dropCanvas(m_dropData.ed.dropElement)) {
                qDebug("cannot drop %s(%p) to canvas", m_dropData.ed.dropElement->name(), m_dropData.ed.dropElement);
                resetDropElement();
            }
            break;
        }
        score()->addRefresh(el->canvasBoundingRect());

        // TODO: HACK ALERT!
        if (el->isMeasure() && m_dropData.ed.dropElement->isLayoutBreak()) {
            Measure* m = toMeasure(el);
            if (m->isMMRest()) {
                el = m->mmRestLast();
            }
        }

        EngravingItem* dropElement = el->drop(m_dropData.ed);
        if (dropElement && dropElement->isInstrumentChange()) {
            selectInstrument(toInstrumentChange(dropElement));
        }
        score()->addRefresh(el->canvasBoundingRect());
        if (dropElement) {
            if (!score()->noteEntryMode()) {
                doSelect({ dropElement }, SelectType::SINGLE);
                needNotifyAboutSelectionChanged = true;
            }
            score()->addRefresh(dropElement->canvasBoundingRect());
        }
        accepted = true;
    }
    break;
    default:
        resetDropElement();
        break;
    }
    m_dropData.ed.dropElement = nullptr;
    setDropTarget(nullptr);         // this also resets dropRectangle and dropAnchor
    apply();
    // update input cursor position (must be done after layout)
//    if (noteEntryMode()) {
//        moveCursor();
//    }
    if (accepted) {
        notifyAboutDropChanged();
    }

    if (needNotifyAboutSelectionChanged) {
        notifyAboutSelectionChanged();
    }

    return accepted;
}

void NotationInteraction::selectInstrument(Ms::InstrumentChange* instrumentChange)
{
    if (!instrumentChange) {
        return;
    }

    RetVal<Instrument> selectedInstrument = selectInstrumentScenario()->selectInstrument();
    if (!selectedInstrument.ret) {
        return;
    }

    instrumentChange->setInit(true);
    instrumentChange->setupInstrument(&selectedInstrument.val);
}

//! NOTE Copied from Palette::applyPaletteElement
bool NotationInteraction::applyPaletteElement(Ms::EngravingItem* element, Qt::KeyboardModifiers modifiers)
{
    IF_ASSERT_FAILED(element) {
        return false;
    }

    Ms::Score* score = this->score();

    if (!score) {
        return false;
    }

    const Ms::Selection sel = score->selection();   // make a copy of selection state before applying the operation.
    if (sel.isNone()) {
        return false;
    }

//#ifdef MSCORE_UNSTABLE
//    if (ScriptRecorder* rec = adapter()->getScriptRecorder()) {
//        if (modifiers == 0) {
//            rec->recordPaletteElement(element);
//        }
//    }
//#endif

    startEdit();

    if (sel.isList()) {
        ChordRest* cr1 = sel.firstChordRest();
        ChordRest* cr2 = sel.lastChordRest();
        bool addSingle = false;           // add a single line only
        if (cr1 && cr2 == cr1) {
            // one chordrest selected, ok to add line
            addSingle = true;
        } else if (sel.elements().size() == 2 && cr1 && cr2 && cr1 != cr2) {
            // two chordrests selected
            // must be on same staff in order to add line, except for slur
            if (element->isSlur() || cr1->staffIdx() == cr2->staffIdx()) {
                addSingle = true;
            }
        }

        auto isEntryDrumStaff = [score]() {
            const Ms::InputState& is = score->inputState();
            Ms::Staff* staff = score->staff(is.track() / Ms::VOICES);
            return staff->staffType(is.tick())->group() == Ms::StaffGroup::PERCUSSION;
        };

        if (isEntryDrumStaff() && element->isChord()) {
            Ms::InputState& is = score->inputState();
            EngravingItem* e = nullptr;
            if (!(modifiers & Qt::ShiftModifier)) {
                // shift+double-click: add note to "chord"
                // use input position rather than selection if possible
                // look for a cr in the voice predefined for the drum in the palette
                // back up if necessary
                // TODO: refactor this with similar code in putNote()
                if (is.segment()) {
                    Ms::Segment* seg = is.segment();
                    while (seg) {
                        if (seg->element(is.track())) {
                            break;
                        }
                        seg = seg->prev(Ms::SegmentType::ChordRest);
                    }
                    if (seg) {
                        is.setSegment(seg);
                    } else {
                        is.setSegment(is.segment()->measure()->first(Ms::SegmentType::ChordRest));
                    }
                }
                score->expandVoice();
                e = is.cr();
            }
            if (!e) {
                e = sel.elements().first();
            }
            if (e) {
                // get note if selection was full chord
                if (e->isChord()) {
                    e = toChord(e)->upNote();
                }

                applyDropPaletteElement(score, e, element, modifiers, PointF(), true);
                // note has already been played (and what would play otherwise may be *next* input position)
                score->setPlayNote(false);
                score->setPlayChord(false);
                // continue in same track
                is.setTrack(e->track());
            } else {
                qDebug("nowhere to place drum note");
            }
        } else if (element->isLayoutBreak()) {
            Ms::LayoutBreak* breakElement = toLayoutBreak(element);
            score->cmdToggleLayoutBreak(breakElement->layoutBreakType());
        } else if (element->isSlur() && addSingle) {
            doAddSlur(toSlur(element));
        } else if (element->isSLine() && !element->isGlissando() && addSingle) {
            Ms::Segment* startSegment = cr1->segment();
            Ms::Segment* endSegment = cr2->segment();
            if (element->type() == Ms::ElementType::PEDAL && cr2 != cr1) {
                endSegment = endSegment->nextCR(cr2->track());
            }
            // TODO - handle cross-voice selections
            int idx = cr1->staffIdx();

            QByteArray a = element->mimeData(PointF());
//printf("<<%s>>\n", a.data());
            Ms::XmlReader e(a);
            Ms::Fraction duration;        // dummy
            PointF dragOffset;
            Ms::ElementType type = Ms::EngravingItem::readType(e, &dragOffset, &duration);
            Ms::Spanner* spanner = static_cast<Ms::Spanner*>(engraving::Factory::createItem(type, score->dummy()));
            spanner->read(e);
            spanner->styleChanged();
            score->cmdAddSpanner(spanner, idx, startSegment, endSegment);
        } else {
            for (EngravingItem* e : sel.elements()) {
                applyDropPaletteElement(score, e, element, modifiers);
            }
        }
    } else if (sel.isRange()) {
        if (element->type() == ElementType::BAR_LINE
            || element->type() == ElementType::MARKER
            || element->type() == ElementType::JUMP
            || element->type() == ElementType::SPACER
            || element->type() == ElementType::VBOX
            || element->type() == ElementType::HBOX
            || element->type() == ElementType::TBOX
            || element->type() == ElementType::MEASURE
            || element->type() == ElementType::BRACKET
            || element->type() == ElementType::STAFFTYPE_CHANGE
            || (element->type() == ElementType::ACTION_ICON
                && (toActionIcon(element)->actionType() == Ms::ActionIconType::VFRAME
                    || toActionIcon(element)->actionType() == Ms::ActionIconType::HFRAME
                    || toActionIcon(element)->actionType() == Ms::ActionIconType::TFRAME
                    || toActionIcon(element)->actionType() == Ms::ActionIconType::MEASURE
                    || toActionIcon(element)->actionType() == Ms::ActionIconType::BRACKETS))) {
            Measure* last = sel.endSegment() ? sel.endSegment()->measure() : nullptr;
            for (Measure* m = sel.startSegment()->measure(); m; m = m->nextMeasureMM()) {
                RectF r = m->staffabbox(sel.staffStart());
                PointF pt(r.x() + r.width() * .5, r.y() + r.height() * .5);
                pt += m->system()->page()->pos();
                applyDropPaletteElement(score, m, element, modifiers, pt);
                if (m == last) {
                    break;
                }
            }
        } else if (element->type() == ElementType::LAYOUT_BREAK) {
            Ms::LayoutBreak* breakElement = static_cast<Ms::LayoutBreak*>(element);
            score->cmdToggleLayoutBreak(breakElement->layoutBreakType());
        } else if (element->isClef() || element->isKeySig() || element->isTimeSig()) {
            Measure* m1 = sel.startSegment()->measure();
            Measure* m2 = sel.endSegment() ? sel.endSegment()->measure() : nullptr;
            if (m2 == m1 && sel.startSegment()->rtick().isZero()) {
                m2 = nullptr;             // don't restore original if one full measure selected
            } else if (m2) {
                m2 = m2->nextMeasureMM();
            }
            // for clefs, apply to each staff separately
            // otherwise just apply to top staff
            int staffIdx1 = sel.staffStart();
            int staffIdx2 = element->type() == ElementType::CLEF ? sel.staffEnd() : staffIdx1 + 1;
            for (int i = staffIdx1; i < staffIdx2; ++i) {
                // for clefs, use mid-measure changes if appropriate
                EngravingItem* e1 = nullptr;
                EngravingItem* e2 = nullptr;
                // use mid-measure clef changes as appropriate
                if (element->type() == ElementType::CLEF) {
                    if (sel.startSegment()->isChordRestType() && sel.startSegment()->rtick().isNotZero()) {
                        ChordRest* cr = static_cast<ChordRest*>(sel.startSegment()->nextChordRest(i * Ms::VOICES));
                        if (cr && cr->isChord()) {
                            e1 = static_cast<Ms::Chord*>(cr)->upNote();
                        } else {
                            e1 = cr;
                        }
                    }
                    if (sel.endSegment() && sel.endSegment()->segmentType() == Ms::SegmentType::ChordRest) {
                        ChordRest* cr = static_cast<ChordRest*>(sel.endSegment()->nextChordRest(i * Ms::VOICES));
                        if (cr && cr->isChord()) {
                            e2 = static_cast<Ms::Chord*>(cr)->upNote();
                        } else {
                            e2 = cr;
                        }
                    }
                }
                if (m2 || e2) {
                    // restore original clef/keysig/timesig
                    Ms::Staff* staff = score->staff(i);
                    Ms::Fraction tick1 = sel.startSegment()->tick();
                    Ms::EngravingItem* oelement = nullptr;
                    switch (element->type()) {
                    case Ms::ElementType::CLEF:
                    {
                        Ms::Clef* oclef = engraving::Factory::createClef(score->dummy()->segment());
                        oclef->setClefType(staff->clef(tick1));
                        oelement = oclef;
                        break;
                    }
                    case Ms::ElementType::KEYSIG:
                    {
                        Ms::KeySig* okeysig = engraving::Factory::createKeySig(score->dummy()->segment());
                        okeysig->setKeySigEvent(staff->keySigEvent(tick1));
                        if (!score->styleB(Ms::Sid::concertPitch) && !okeysig->isCustom() && !okeysig->isAtonal()) {
                            Ms::Interval v = staff->part()->instrument(tick1)->transpose();
                            if (!v.isZero()) {
                                Key k = okeysig->key();
                                okeysig->setKey(transposeKey(k, v, okeysig->part()->preferSharpFlat()));
                            }
                        }
                        oelement = okeysig;
                        break;
                    }
                    case Ms::ElementType::TIMESIG:
                    {
                        Ms::TimeSig* otimesig = engraving::Factory::createTimeSig(score->dummy()->segment());
                        otimesig->setFrom(staff->timeSig(tick1));
                        oelement = otimesig;
                        break;
                    }
                    default:
                        break;
                    }
                    if (oelement) {
                        if (e2) {
                            applyDropPaletteElement(score, e2, oelement, modifiers);
                        } else {
                            RectF r = m2->staffabbox(i);
                            PointF pt(r.x() + r.width() * .5, r.y() + r.height() * .5);
                            pt += m2->system()->page()->pos();
                            applyDropPaletteElement(score, m2, oelement, modifiers, pt);
                        }
                        delete oelement;
                    }
                }
                // apply new clef/keysig/timesig
                if (e1) {
                    applyDropPaletteElement(score, e1, element, modifiers);
                } else {
                    RectF r = m1->staffabbox(i);
                    PointF pt(r.x() + r.width() * .5, r.y() + r.height() * .5);
                    pt += m1->system()->page()->pos();
                    applyDropPaletteElement(score, m1, element, modifiers, pt);
                }
            }
        } else if (element->isSlur()) {
            doAddSlur(toSlur(element));
        } else if (element->isSLine() && element->type() != ElementType::GLISSANDO) {
            Ms::Segment* startSegment = sel.startSegment();
            Ms::Segment* endSegment = sel.endSegment();
            bool firstStaffOnly = element->isVolta() && !(modifiers & Qt::ControlModifier);
            int startStaff = firstStaffOnly ? 0 : sel.staffStart();
            int endStaff   = firstStaffOnly ? 1 : sel.staffEnd();
            for (int i = startStaff; i < endStaff; ++i) {
                Ms::Spanner* spanner = static_cast<Ms::Spanner*>(element->clone());
                spanner->setScore(score);
                spanner->styleChanged();
                score->cmdAddSpanner(spanner, i, startSegment, endSegment);
            }
        } else if (element->isTextBase()) {
            Ms::Segment* firstSegment = sel.startSegment();
            int firstStaffIndex = sel.staffStart();
            int lastStaffIndex = sel.staffEnd();

            // A text should only be added at the start of the selection
            // There shouldn't be a text at each element
            for (int staff = firstStaffIndex; staff < lastStaffIndex; staff++) {
                applyDropPaletteElement(score, firstSegment->firstElement(staff), element, modifiers);
            }
        } else {
            int track1 = sel.staffStart() * Ms::VOICES;
            int track2 = sel.staffEnd() * Ms::VOICES;
            Ms::Segment* startSegment = sel.startSegment();
            Ms::Segment* endSegment = sel.endSegment();       //keep it, it could change during the loop

            for (Ms::Segment* s = startSegment; s && s != endSegment; s = s->next1()) {
                for (int track = track1; track < track2; ++track) {
                    Ms::EngravingItem* e = s->element(track);
                    if (e == 0 || !score->selectionFilter().canSelect(e)
                        || !score->selectionFilter().canSelectVoice(track)) {
                        continue;
                    }
                    if (e->isChord()) {
                        Ms::Chord* chord = toChord(e);
                        for (Ms::Note* n : chord->notes()) {
                            applyDropPaletteElement(score, n, element, modifiers);
                            if (!(element->isAccidental() || element->isNoteHead())) {             // only these need to apply to every note
                                break;
                            }
                        }
                    } else {
                        // do not apply articulation to barline in a range selection
                        if (!e->isBarLine() || !element->isArticulation()) {
                            applyDropPaletteElement(score, e, element, modifiers);
                        }
                    }
                }
                if (!element->placeMultiple()) {
                    break;
                }
            }
        }
    } else {
        qDebug("unknown selection state");
    }

    apply();

    setDropTarget(nullptr);

    return true;
}

//! NOTE Copied from Palette applyDrop
void NotationInteraction::applyDropPaletteElement(Ms::Score* score, Ms::EngravingItem* target, Ms::EngravingItem* e,
                                                  Qt::KeyboardModifiers modifiers,
                                                  PointF pt, bool pasteMode)
{
    Ms::EditData newData(&m_scoreCallbacks);
    Ms::EditData* dropData = &newData;

    if (isTextEditingStarted()) {
        dropData = &m_editData;
    }

    dropData->pos         = pt.isNull() ? target->pagePos() : pt;
    dropData->dragOffset  = QPointF();
    dropData->modifiers   = modifiers;
    dropData->dropElement = e;

    if (target->acceptDrop(*dropData)) {
        // use same code path as drag&drop

        QByteArray a = e->mimeData(PointF());

        Ms::XmlReader n(a);
        n.setPasteMode(pasteMode);
        Fraction duration;      // dummy
        PointF dragOffset;
        ElementType type = EngravingItem::readType(n, &dragOffset, &duration);
        dropData->dropElement = engraving::Factory::createItem(type, score->dummy());

        dropData->dropElement->read(n);
        dropData->dropElement->styleChanged();       // update to local style

        Ms::EngravingItem* el = target->drop(*dropData);
        if (el && el->isInstrumentChange()) {
            selectInstrument(toInstrumentChange(el));
        }

        if (el && !score->inputState().noteEntryMode()) {
            doSelect({ el }, Ms::SelectType::SINGLE, 0);
        }
        dropData->dropElement = nullptr;

        m_notifyAboutDropChanged = true;
    }
}

//! NOTE Copied from ScoreView::cmdAddSlur
void NotationInteraction::doAddSlur(const Ms::Slur* slurTemplate)
{
    startEdit();
    m_notifyAboutDropChanged = true;

    Ms::ChordRest* firstChordRest = nullptr;
    Ms::ChordRest* secondChordRest = nullptr;
    const auto& sel = score()->selection();
    auto el = sel.uniqueElements();

    if (sel.isRange()) {
        int startTrack = sel.staffStart() * Ms::VOICES;
        int endTrack = sel.staffEnd() * Ms::VOICES;
        for (int track = startTrack; track < endTrack; ++track) {
            firstChordRest = nullptr;
            secondChordRest = nullptr;
            for (Ms::EngravingItem* e : el) {
                if (e->track() != track) {
                    continue;
                }
                if (e->isNote()) {
                    e = toNote(e)->chord();
                }
                if (!e->isChord()) {
                    continue;
                }
                Ms::ChordRest* cr = Ms::toChordRest(e);
                if (!firstChordRest || firstChordRest->tick() > cr->tick()) {
                    firstChordRest = cr;
                }
                if (!secondChordRest || secondChordRest->tick() < cr->tick()) {
                    secondChordRest = cr;
                }
            }

            if (firstChordRest && (firstChordRest != secondChordRest)) {
                doAddSlur(firstChordRest, secondChordRest, slurTemplate);
            }
        }
    } else if (sel.isSingle()) {
        if (sel.element()->isNote()) {
            doAddSlur(toNote(sel.element())->chord(), nullptr, slurTemplate);
        }
    } else {
        for (Ms::EngravingItem* e : el) {
            if (e->isNote()) {
                e = Ms::toNote(e)->chord();
            }
            if (!e->isChord()) {
                continue;
            }
            Ms::ChordRest* cr = Ms::toChordRest(e);
            if (!firstChordRest || cr->isBefore(firstChordRest)) {
                firstChordRest = cr;
            }
            if (!secondChordRest || secondChordRest->isBefore(cr)) {
                secondChordRest = cr;
            }
        }

        if (firstChordRest == secondChordRest) {
            secondChordRest = Ms::nextChordRest(firstChordRest);
        }

        if (firstChordRest) {
            doAddSlur(firstChordRest, secondChordRest, slurTemplate);
        }
    }

    apply();
}

void NotationInteraction::doAddSlur(ChordRest* firstChordRest, ChordRest* secondChordRest, const Ms::Slur* slurTemplate)
{
    Ms::Slur* slur = firstChordRest->slur(secondChordRest);
    if (!slur || slur->slurDirection() != Ms::DirectionV::AUTO) {
        slur = score()->addSlur(firstChordRest, secondChordRest, slurTemplate);
    }
    if (m_noteInput->isNoteInputMode()) {
        m_noteInput->addSlur(slur);
    } else if (!secondChordRest) {
        select({ slur->frontSegment() }, SelectType::SINGLE);
        updateGripEdit();
    }
}

bool NotationInteraction::scoreHasMeasure() const
{
    Ms::Page* page = score()->pages().isEmpty() ? nullptr : score()->pages().front();
    const QList<Ms::System*>* systems = page ? &page->systems() : nullptr;
    if (systems == nullptr || systems->empty() || systems->front()->measures().empty()) {
        return false;
    }

    return true;
}

bool NotationInteraction::notesHaveActiculation(const std::vector<Note*>& notes, SymbolId articulationSymbolId) const
{
    for (Note* note: notes) {
        Chord* chord = note->chord();

        std::set<SymbolId> chordArticulations = chord->articulationSymbolIds();
        chordArticulations = Ms::flipArticulations(chordArticulations, Ms::PlacementV::ABOVE);
        chordArticulations = Ms::splitArticulations(chordArticulations);

        if (chordArticulations.find(articulationSymbolId) == chordArticulations.end()) {
            return false;
        }
    }

    return true;
}

//! NOTE Copied from ScoreView::dragLeaveEvent
void NotationInteraction::endDrop()
{
    if (m_dropData.ed.dropElement) {
        score()->setUpdateAll();
        resetDropElement();
        score()->update();
    }
    setDropTarget(nullptr);
}

mu::async::Notification NotationInteraction::dropChanged() const
{
    return m_dropChanged;
}

//! NOTE Copied from ScoreView::dropCanvas
bool NotationInteraction::dropCanvas(EngravingItem* e)
{
    if (e->isActionIcon()) {
        switch (Ms::toActionIcon(e)->actionType()) {
        case Ms::ActionIconType::VFRAME:
            score()->insertMeasure(ElementType::VBOX, 0);
            break;
        case Ms::ActionIconType::HFRAME:
            score()->insertMeasure(ElementType::HBOX, 0);
            break;
        case Ms::ActionIconType::TFRAME:
            score()->insertMeasure(ElementType::TBOX, 0);
            break;
        case Ms::ActionIconType::FFRAME:
            score()->insertMeasure(ElementType::FBOX, 0);
            break;
        case Ms::ActionIconType::MEASURE:
            score()->insertMeasure(ElementType::MEASURE, 0);
            break;
        default:
            return false;
        }
        delete e;
        return true;
    }
    return false;
}

//! NOTE Copied from ScoreView::getDropTarget
EngravingItem* NotationInteraction::dropTarget(Ms::EditData& ed) const
{
    QList<EngravingItem*> el = elementsAt(ed.pos);
    for (EngravingItem* e : el) {
        if (e->isStaffLines()) {
            if (el.size() > 2) {          // is not first class drop target
                continue;
            }
            e = Ms::toStaffLines(e)->measure();
        }
        if (e->acceptDrop(ed)) {
            return e;
        }
    }
    return nullptr;
}

//! NOTE Copied from ScoreView::dragMeasureAnchorElement
bool NotationInteraction::dragMeasureAnchorElement(const PointF& pos)
{
    int staffIdx;
    Ms::Segment* seg;
    Ms::MeasureBase* mb = score()->pos2measure(pos, &staffIdx, 0, &seg, 0);
    if (!(m_dropData.ed.modifiers & Qt::ControlModifier)) {
        staffIdx = 0;
    }
    int track = staffIdx * Ms::VOICES;

    if (mb && mb->isMeasure()) {
        Ms::Measure* m = Ms::toMeasure(mb);
        Ms::System* s  = m->system();
        qreal y    = s->staff(staffIdx)->y() + s->pos().y() + s->page()->pos().y();
        RectF b(m->canvasBoundingRect());
        if (pos.x() >= (b.x() + b.width() * .5) && m != score()->lastMeasureMM()
            && m->nextMeasure()->system() == m->system()) {
            m = m->nextMeasure();
        }
        PointF anchor(m->canvasBoundingRect().x(), y);
        setAnchorLines({ LineF(pos, anchor) });
        m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
        m_dropData.ed.dropElement->setTrack(track);
        m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
        m_notifyAboutDropChanged = true;
        return true;
    }
    m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
    setDropTarget(nullptr);
    return false;
}

//! NOTE Copied from ScoreView::dragTimeAnchorElement
bool NotationInteraction::dragTimeAnchorElement(const PointF& pos)
{
    int staffIdx;
    Ms::Segment* seg;
    Ms::MeasureBase* mb = score()->pos2measure(pos, &staffIdx, 0, &seg, 0);
    int track = staffIdx * Ms::VOICES;

    if (mb && mb->isMeasure() && seg->element(track)) {
        Ms::Measure* m = Ms::toMeasure(mb);
        Ms::System* s  = m->system();
        qreal y    = s->staff(staffIdx)->y() + s->pos().y() + s->page()->pos().y();
        PointF anchor(seg->canvasBoundingRect().x(), y);
        setAnchorLines({ LineF(pos, anchor) });
        m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
        m_dropData.ed.dropElement->setTrack(track);
        m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
        notifyAboutDragChanged();
        return true;
    }
    m_dropData.ed.dropElement->score()->addRefresh(m_dropData.ed.dropElement->canvasBoundingRect());
    setDropTarget(nullptr);
    return false;
}

//! NOTE Copied from ScoreView::setDropTarget
void NotationInteraction::setDropTarget(EngravingItem* el)
{
    if (m_dropData.dropTarget != el) {
        if (m_dropData.dropTarget) {
            m_dropData.dropTarget->setDropTarget(false);
            m_dropData.dropTarget = nullptr;
        }

        m_dropData.dropTarget = el;
        if (m_dropData.dropTarget) {
            m_dropData.dropTarget->setDropTarget(true);
        }
    }

    m_anchorLines.clear();

    //! TODO
    //    if (dropRectangle.isValid()) {
    //        dropRectangle = QRectF();
    //    }
    //! ---

    notifyAboutDragChanged();
}

void NotationInteraction::resetDropElement()
{
    if (m_dropData.ed.dropElement) {
        delete m_dropData.ed.dropElement;
        m_dropData.ed.dropElement = nullptr;
    }
}

void NotationInteraction::setAnchorLines(const std::vector<LineF>& anchorList)
{
    m_anchorLines = anchorList;
}

void NotationInteraction::resetAnchorLines()
{
    m_anchorLines.clear();
}

double NotationInteraction::currentScaling(mu::draw::Painter* painter) const
{
    qreal guiScaling = configuration()->guiScaling();
    return painter->worldTransform().m11() / guiScaling;
}

void NotationInteraction::drawAnchorLines(mu::draw::Painter* painter)
{
    if (m_anchorLines.empty()) {
        return;
    }

    const auto dropAnchorColor = configuration()->anchorLineColor();
    mu::draw::Pen pen(dropAnchorColor, 2.0 / currentScaling(painter), mu::draw::PenStyle::DotLine);

    for (const LineF& anchor : m_anchorLines) {
        painter->setPen(pen);
        painter->drawLine(anchor);

        qreal d = 4.0 / currentScaling(painter);
        RectF rect(-d, -d, 2 * d, 2 * d);

        painter->setBrush(mu::draw::Brush(dropAnchorColor));
        painter->setNoPen();
        rect.moveCenter(anchor.p1());
        painter->drawEllipse(rect);
        rect.moveCenter(anchor.p2());
        painter->drawEllipse(rect);
    }
}

void NotationInteraction::drawTextEditMode(draw::Painter* painter)
{
    if (!isTextEditingStarted()) {
        return;
    }

    m_editData.element->drawEditMode(painter, m_editData, currentScaling(painter));
}

void NotationInteraction::drawSelectionRange(draw::Painter* painter)
{
    using namespace draw;
    if (!m_selection->isRange()) {
        return;
    }

    painter->setBrush(BrushStyle::NoBrush);

    QColor selectionColor = configuration()->selectionColor();
    qreal penWidth = 3.0 / currentScaling(painter);

    Pen pen;
    pen.setColor(selectionColor);
    pen.setWidthF(penWidth);
    pen.setStyle(PenStyle::SolidLine);
    painter->setPen(pen);

    std::vector<RectF> rangeArea = m_selection->range()->boundingArea();
    for (const RectF& rect: rangeArea) {
        mu::PainterPath path;
        path.addRoundedRect(rect, 6, 6);

        QColor fillColor = selectionColor;
        fillColor.setAlpha(10);
        painter->fillPath(path, fillColor);
        painter->drawPath(path);
    }
}

void NotationInteraction::drawGripPoints(draw::Painter* painter)
{
    if (!selection()->element()) {
        return;
    }
    bool editingElement = m_editData.element != nullptr;
    if (!editingElement) {
        m_editData.element = selection()->element();
        if (!m_editData.element->isTextBase() && !m_editData.getData(m_editData.element)) {
            m_editData.element->startEdit(m_editData);
        }
    }
    m_editData.grips = m_editData.element->gripsCount();
    m_editData.grip.resize(m_editData.grips);

    constexpr qreal DEFAULT_GRIP_SIZE = 8;
    qreal scaling = currentScaling(painter);
    qreal gripSize = DEFAULT_GRIP_SIZE / scaling;
    RectF newRect(-gripSize / 2, -gripSize / 2, gripSize, gripSize);

    EngravingItem* page = m_editData.element->findAncestor(ElementType::PAGE);
    PointF pageOffset = page ? page->pos() : m_editData.element->pos();

    for (RectF& gripRect: m_editData.grip) {
        gripRect = newRect.translated(pageOffset);
    }

    m_editData.element->updateGrips(m_editData);
    m_editData.element->drawEditMode(painter, m_editData, currentScaling(painter));
    if (!editingElement) {
        m_editData.element = nullptr;
    }
}

ChordRest* activeCr(Ms::Score* score)
{
    ChordRest* cr = score->selection().activeCR();
    if (!cr) {
        cr = score->selection().lastChordRest();
        if (!cr && score->noteEntryMode()) {
            cr = score->inputState().cr();
        }
    }
    return cr;
}

void NotationInteraction::expandSelection(ExpandSelectionMode mode)
{
    ChordRest* cr = activeCr(score());
    if (!cr) {
        return;
    }
    ChordRest* el = 0;
    switch (mode) {
    case ExpandSelectionMode::BeginSystem: {
        Measure* measure = cr->segment()->measure()->system()->firstMeasure();
        if (measure) {
            el = measure->first()->nextChordRest(cr->track());
        }
        break;
    }
    case ExpandSelectionMode::EndSystem: {
        Measure* measure = cr->segment()->measure()->system()->lastMeasure();
        if (measure) {
            el = measure->last()->nextChordRest(cr->track(), true);
        }
        break;
    }
    case ExpandSelectionMode::BeginScore: {
        Measure* measure = score()->firstMeasureMM();
        if (measure) {
            el = measure->first()->nextChordRest(cr->track());
        }
        break;
    }
    case ExpandSelectionMode::EndScore: {
        Measure* measure = score()->lastMeasureMM();
        if (measure) {
            el = measure->last()->nextChordRest(cr->track(), true);
        }
        break;
    }
    }
    if (el) {
        score()->select(el, SelectType::RANGE, el->staffIdx());
        notifyAboutSelectionChanged();
    }
}

void NotationInteraction::addToSelection(MoveDirection d, MoveSelectionType type)
{
    ChordRest* cr = activeCr(score());
    if (!cr) {
        return;
    }
    ChordRest* el = 0;
    switch (type) {
    case MoveSelectionType::Chord:
        if (d == MoveDirection::Right) {
            el = Ms::nextChordRest(cr, true);
        } else {
            el = Ms::prevChordRest(cr, true);
        }
        break;
    case MoveSelectionType::Measure:
        if (d == MoveDirection::Right) {
            el = score()->nextMeasure(cr, true, true);
        } else {
            el = score()->prevMeasure(cr, true);
        }
        break;
    case MoveSelectionType::Track:
        if (d == MoveDirection::Up) {
            el = score()->upStaff(cr);
        } else {
            el = score()->downStaff(cr);
        }
    case MoveSelectionType::EngravingItem:
    case MoveSelectionType::Frame:
    case MoveSelectionType::System:
    case MoveSelectionType::String:
    case MoveSelectionType::Undefined:
        break;
    }
    if (el) {
        score()->select(el, SelectType::RANGE, el->staffIdx());
        notifyAboutSelectionChanged();
    }
}

void NotationInteraction::moveSelection(MoveDirection d, MoveSelectionType type)
{
    IF_ASSERT_FAILED(MoveSelectionType::Undefined != type) {
        return;
    }

    if (type != MoveSelectionType::String) {
        IF_ASSERT_FAILED(MoveDirection::Left == d || MoveDirection::Right == d) {
            return;
        }
    } else {
        IF_ASSERT_FAILED(MoveDirection::Up == d || MoveDirection::Down == d) {
            return;
        }
    }

    if (MoveSelectionType::EngravingItem == type) {
        moveElementSelection(d);
        return;
    }

    if (MoveSelectionType::String == type) {
        moveStringSelection(d);
        return;
    }

    // TODO: rewrite, Score::move( ) logic needs to be here

    auto typeToString = [](MoveSelectionType type) {
        switch (type) {
        case MoveSelectionType::Undefined: return QString();
        case MoveSelectionType::EngravingItem:   return QString();
        case MoveSelectionType::Chord:     return QString("chord");
        case MoveSelectionType::Measure:   return QString("measure");
        case MoveSelectionType::Track:     return QString("track");
        case MoveSelectionType::Frame:     return QString("frame");
        case MoveSelectionType::System:    return QString("system");
        case MoveSelectionType::String:   return QString();
        }
        return QString();
    };

    QString cmd;
    if (MoveDirection::Left == d) {
        cmd = "prev-";
    } else if (MoveDirection::Right == d) {
        cmd = "next-";
    }

    cmd += typeToString(type);

    score()->move(cmd);
    notifyAboutSelectionChanged();
}

void NotationInteraction::selectTopStaff()
{
    EngravingItem* el = score()->cmdTopStaff(activeCr(score()));
    if (score()->noteEntryMode()) {
        score()->inputState().moveInputPos(el);
    }
    score()->select(el, SelectType::SINGLE, 0);
    notifyAboutSelectionChanged();
}

void NotationInteraction::selectEmptyTrailingMeasure()
{
    ChordRest* cr = activeCr(score());
    const Measure* ftm = score()->firstTrailingMeasure(cr ? &cr : nullptr);
    if (!ftm) {
        ftm = score()->lastMeasure();
    }
    if (ftm) {
        if (score()->styleB(Ms::Sid::createMultiMeasureRests) && ftm->hasMMRest()) {
            ftm = ftm->mmRest1();
        }
        EngravingItem* el
            = !cr ? ftm->first()->nextChordRest(0, false) : ftm->first()->nextChordRest(Ms::trackZeroVoice(cr->track()), false);
        score()->inputState().moveInputPos(el);
        select({ el }, SelectType::SINGLE);
    }
}

static ChordRest* asChordRest(EngravingItem* e)
{
    if (e && e->isNote()) {
        return toNote(e)->chord();
    } else if (e && e->isChordRest()) {
        return toChordRest(e);
    }
    return nullptr;
}

void NotationInteraction::moveChordRestToStaff(MoveDirection dir)
{
    startEdit();
    for (EngravingItem* e: score()->selection().uniqueElements()) {
        ChordRest* cr = asChordRest(e);
        if (cr != nullptr) {
            if (dir == MoveDirection::Up) {
                score()->moveUp(cr);
            } else if (dir == MoveDirection::Down) {
                score()->moveDown(cr);
            }
        }
    }
    apply();
    notifyAboutNotationChanged();
}

void NotationInteraction::swapChordRest(MoveDirection direction)
{
    ChordRest* cr = asChordRest(score()->getSelectedElement());
    if (!cr) {
        return;
    }
    QList<ChordRest*> crl;
    if (cr->links()) {
        for (auto* l : *cr->links()) {
            crl.append(toChordRest(l));
        }
    } else {
        crl.append(cr);
    }
    startEdit();
    for (ChordRest* cr1 : crl) {
        if (cr1->type() == ElementType::REST) {
            Measure* m = toRest(cr1)->measure();
            if (m && m->isMMRest()) {
                break;
            }
        }
        ChordRest* cr2;
        // ensures cr1 is the left chord, useful in SwapCR::flip()
        if (direction == MoveDirection::Left) {
            cr2 = cr1;
            cr1 = prevChordRest(cr2);
        } else {
            cr2 = nextChordRest(cr1);
        }
        if (cr1 && cr2 && cr1->measure() == cr2->measure() && !cr1->tuplet() && !cr2->tuplet()
            && cr1->durationType() == cr2->durationType() && cr1->ticks() == cr2->ticks()
            // if two chords belong to different two-note tremolos, abort
            && !(cr1->isChord() && toChord(cr1)->tremolo() && toChord(cr1)->tremolo()->twoNotes()
                 && cr2->isChord() && toChord(cr2)->tremolo() && toChord(cr2)->tremolo()->twoNotes()
                 && toChord(cr1)->tremolo() != toChord(cr2)->tremolo())) {
            score()->undo(new Ms::SwapCR(cr1, cr2));
        }
    }
    apply();
}

void NotationInteraction::moveElementSelection(MoveDirection d)
{
    EngravingItem* el = score()->selection().element();
    if (!el && !score()->selection().elements().isEmpty()) {
        el = score()->selection().elements().last();
    }

    if (!el) {
        ChordRest* cr = score()->selection().currentCR();
        if (cr) {
            if (cr->isChord()) {
                if (MoveDirection::Left == d) {
                    el = toChord(cr)->upNote();
                } else {
                    el = toChord(cr)->downNote();
                }
            } else if (cr->isRest()) {
                el = cr;
            }
            score()->select(el);
        }
    }

    EngravingItem* toEl = nullptr;
    if (el) {
        toEl = (MoveDirection::Left == d) ? score()->prevElement() : score()->nextElement();
    } else {
        toEl = (MoveDirection::Left == d) ? score()->lastElement() : score()->firstElement();
    }

    if (!toEl) {
        return;
    }

    select({ toEl }, SelectType::SINGLE);

    if (toEl->type() == ElementType::NOTE || toEl->type() == ElementType::HARMONY) {
        score()->setPlayNote(true);
    }
}

void NotationInteraction::moveStringSelection(MoveDirection d)
{
    Ms::InputState& is = score()->inputState();
    Ms::Staff* staff = score()->staff(is.track() / Ms::VOICES);
    int instrStrgs = staff->part()->instrument(is.tick())->stringData()->strings();
    int delta = (staff->staffType(is.tick())->upsideDown() ? -1 : 1);

    if (MoveDirection::Up == d) {
        delta = -delta;
    }

    int strg = is.string() + delta;
    if (strg >= 0 && strg < instrStrgs) {
        is.setString(strg);
        notifyAboutSelectionChanged();
    }
}

inline Ms::DirectionV toDirection(MoveDirection d)
{
    return d == MoveDirection::Up ? Ms::DirectionV::UP : Ms::DirectionV::DOWN;
}

void NotationInteraction::movePitch(MoveDirection d, PitchMode mode)
{
    IF_ASSERT_FAILED(MoveDirection::Up == d || MoveDirection::Down == d) {
        return;
    }

    startEdit();

    if (score()->selection().element() && score()->selection().element()->isRest()) {
        score()->cmdMoveRest(toRest(score()->selection().element()), toDirection(d));
    } else {
        score()->upDown(MoveDirection::Up == d, mode);
    }

    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::moveLyrics(MoveDirection d)
{
    EngravingItem* el = score()->selection().element();
    IF_ASSERT_FAILED(el && el->isLyrics()) {
        return;
    }
    startEdit();
    score()->cmdMoveLyrics(toLyrics(el), toDirection(d));
    apply();
}

void NotationInteraction::nudge(MoveDirection d, bool quickly)
{
    EngravingItem* el = score()->selection().element();
    IF_ASSERT_FAILED(el && (el->isTextBase() || el->isArticulation())) {
        return;
    }

    startEdit();

    qreal step = quickly ? Ms::MScore::nudgeStep10 : Ms::MScore::nudgeStep;
    step = step * el->spatium();

    switch (d) {
    case MoveDirection::Undefined:
        IF_ASSERT_FAILED(d != MoveDirection::Undefined) {
            return;
        }
        break;
    case MoveDirection::Left:
        el->undoChangeProperty(Ms::Pid::OFFSET, el->offset() - PointF(step, 0.0), Ms::PropertyFlags::UNSTYLED);
        break;
    case MoveDirection::Right:
        el->undoChangeProperty(Ms::Pid::OFFSET, el->offset() + PointF(step, 0.0), Ms::PropertyFlags::UNSTYLED);
        break;
    case MoveDirection::Up:
        el->undoChangeProperty(Ms::Pid::OFFSET, el->offset() - PointF(0.0, step), Ms::PropertyFlags::UNSTYLED);
        break;
    case MoveDirection::Down:
        el->undoChangeProperty(Ms::Pid::OFFSET, el->offset() + PointF(0.0, step), Ms::PropertyFlags::UNSTYLED);
        break;
    }

    apply();

    notifyAboutDragChanged();
}

bool NotationInteraction::isTextSelected() const
{
    EngravingItem* selectedElement = m_selection->element();
    if (!selectedElement) {
        return false;
    }

    if (!selectedElement->isTextBase()) {
        return false;
    }

    return true;
}

bool NotationInteraction::isTextEditingStarted() const
{
    return m_editData.element && m_editData.element->isTextBase();
}

bool NotationInteraction::textEditingAllowed(const EngravingItem* element) const
{
    return element && element->isEditable() && (element->isTextBase() || element->isTBox());
}

void NotationInteraction::startEditText(EngravingItem* element, const PointF& cursorPos)
{
    if (!element) {
        return;
    }

    m_editData.startMove = cursorPos;

    if (isTextEditingStarted()) {
        // double click on a textBase element that is being edited - select word
        Ms::TextBase* textBase = Ms::toTextBase(m_editData.element);
        textBase->multiClickSelect(m_editData, Ms::MultiClick::Double);
        textBase->endHexState(m_editData);
        textBase->setPrimed(false);
    } else {
        m_editData.clearData();

        if (element->isTBox()) {
            m_editData.element = toTBox(element)->text();
        } else {
            m_editData.element = element;
        }

        m_editData.element->startEdit(m_editData);
    }

    notifyAboutTextEditingStarted();
    notifyAboutTextEditingChanged();
}

static qreal nudgeDistance(const Ms::EditData& editData)
{
    qreal spatium = editData.element->spatium();
    if (editData.element->isBeam()) {
        if (editData.modifiers & Qt::ControlModifier) {
            return spatium;
        } else if (editData.modifiers & Qt::AltModifier) {
            return spatium * 4;
        } else {
            return spatium * 0.25;
        }
    } else {
        if (editData.modifiers & Qt::ControlModifier) {
            return spatium * Ms::MScore::nudgeStep10;
        } else if (editData.modifiers & Qt::AltModifier) {
            return spatium * Ms::MScore::nudgeStep50;
        } else {
            return spatium * Ms::MScore::nudgeStep;
        }
    }
}

static qreal nudgeDistance(const Ms::EditData& editData, qreal raster)
{
    qreal distance = nudgeDistance(editData);
    if (raster > 0) {
        raster = editData.element->spatium() / raster;
        if (distance < raster) {
            distance = raster;
        }
    }
    return distance;
}

bool NotationInteraction::handleKeyPress(QKeyEvent* event)
{
    if (event->modifiers() == Qt::KeyboardModifier::AltModifier && !isElementEditStarted()) {
        return false;
    }
    if (m_editData.element->isTextBase()) {
        return false;
    }
    qreal vRaster = Ms::MScore::vRaster(), hRaster = Ms::MScore::hRaster();
    switch (event->key()) {
    case Qt::Key_Tab:
        if (!m_editData.element->nextGrip(m_editData)) {
            return false;
        }
        updateAnchorLines();
        return true;
    case Qt::Key_Backtab:
        if (!m_editData.element->prevGrip(m_editData)) {
            return false;
        }
        updateAnchorLines();
        return true;
    case Qt::Key_Left:
        m_editData.delta = QPointF(-nudgeDistance(m_editData, hRaster), 0);
        break;
    case Qt::Key_Right:
        m_editData.delta = QPointF(nudgeDistance(m_editData, hRaster), 0);
        break;
    case Qt::Key_Up:
        m_editData.delta = QPointF(0, -nudgeDistance(m_editData, vRaster));
        break;
    case Qt::Key_Down:
        m_editData.delta = QPointF(0, nudgeDistance(m_editData, vRaster));
        break;
    default:
        return false;
    }
    m_editData.evtDelta = m_editData.moveDelta = m_editData.delta;
    m_editData.hRaster = hRaster;
    m_editData.vRaster = vRaster;
    if (m_editData.curGrip == Ms::Grip::NO_GRIP) {
        m_editData.curGrip = m_editData.element->defaultGrip();
    }
    if (m_editData.curGrip != Ms::Grip::NO_GRIP && int(m_editData.curGrip) < m_editData.grips) {
        m_editData.pos = m_editData.grip[int(m_editData.curGrip)].center() + m_editData.delta;
    }
    m_editData.element->startEditDrag(m_editData);
    m_editData.element->editDrag(m_editData);
    m_editData.element->endEditDrag(m_editData);
    return true;
}

void NotationInteraction::endEditText()
{
    IF_ASSERT_FAILED(m_editData.element) {
        return;
    }

    if (!isTextEditingStarted()) {
        return;
    }

    doEndTextEdit();

    notifyAboutTextEditingChanged();
    notifyAboutSelectionChanged();
}

void NotationInteraction::changeTextCursorPosition(const PointF& newCursorPos)
{
    IF_ASSERT_FAILED(isTextEditingStarted() && m_editData.element) {
        return;
    }

    m_editData.startMove = newCursorPos;

    Ms::TextBase* textEl = Ms::toTextBase(m_editData.element);

    textEl->mousePress(m_editData);
    if (m_editData.buttons == Qt::MiddleButton) {
        #if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        QClipboard::Mode mode = QClipboard::Clipboard;
        #else
        QClipboard::Mode mode = QClipboard::Selection;
        #endif
        QString txt = QGuiApplication::clipboard()->text(mode);
        textEl->paste(m_editData, txt);
    }

    notifyAboutTextEditingChanged();
}

const TextBase* NotationInteraction::editedText() const
{
    return Ms::toTextBase(m_editData.element);
}

void NotationInteraction::undo()
{
    m_undoStack->undo(&m_editData);
    notifyAboutSelectionChanged();
}

void NotationInteraction::redo()
{
    m_undoStack->redo(&m_editData);
    notifyAboutSelectionChanged();
}

mu::async::Notification NotationInteraction::textEditingStarted() const
{
    return m_textEditingStarted;
}

mu::async::Notification NotationInteraction::textEditingChanged() const
{
    return m_textEditingChanged;
}

mu::async::Channel<ScoreConfigType> NotationInteraction::scoreConfigChanged() const
{
    return m_scoreConfigChanged;
}

bool NotationInteraction::isGripEditStarted() const
{
    return m_editData.element && m_editData.curGrip != Ms::Grip::NO_GRIP;
}

static int findGrip(const QVector<mu::RectF>& grips, const mu::PointF& canvasPos)
{
    if (grips.empty()) {
        return -1;
    }
    qreal align = grips[0].width() / 2;
    for (int i = 0; i < grips.size(); ++i) {
        if (grips[i].adjusted(-align, -align, align, align).contains(canvasPos)) {
            return i;
        }
    }
    return -1;
}

bool NotationInteraction::isHitGrip(const PointF& pos) const
{
    return selection()->element() && findGrip(m_editData.grip, pos) != -1;
}

void NotationInteraction::startEditGrip(const PointF& pos)
{
    int grip = findGrip(m_editData.grip, pos);
    if (grip == -1) {
        return;
    }
    startEditGrip(Ms::Grip(grip));
}

void NotationInteraction::updateAnchorLines()
{
    std::vector<LineF> lines;
    Ms::Grip anchorLinesGrip = m_editData.curGrip == Ms::Grip::NO_GRIP ? m_editData.element->defaultGrip() : m_editData.curGrip;
    QVector<LineF> anchorLines = m_editData.element->gripAnchorLines(anchorLinesGrip);

    if (!anchorLines.isEmpty()) {
        for (LineF& line : anchorLines) {
            if (line.p1() != line.p2()) {
                lines.push_back(line);
            }
        }
    }

    setAnchorLines(lines);
}

void NotationInteraction::startEditGrip(Ms::Grip grip)
{
    if (m_editData.element == nullptr) {
        m_editData.element = score()->selection().element();
    }
    m_editData.curGrip = Ms::Grip(grip);
    updateAnchorLines();
    m_editData.element->startEdit(m_editData);
    notifyAboutNotationChanged();
}

bool NotationInteraction::isElementEditStarted() const
{
    return m_editData.element != nullptr && (m_editData.grips == 0 || m_editData.curGrip != Ms::Grip::NO_GRIP);
}

void NotationInteraction::startEditElement(EngravingItem* element)
{
    if (!element) {
        return;
    }

    if (isElementEditStarted()) {
        return;
    }

    if (element->isTextBase()) {
        startEditText(element, PointF());
    } else if (m_editData.grips > 1) {
        startEditGrip(Ms::Grip::END);
        if (m_editData.element->generated()) {
            m_editData.element = nullptr;
        }
    } else if (element->isEditable()) {
        element->startEdit(m_editData);
        m_editData.element = element;
    }
}

void NotationInteraction::editElement(QKeyEvent* event)
{
    m_editData.modifiers = event->modifiers();
    if (isDragStarted()) {
        return; // ignore all key strokes while dragging
    }
    bool wasEditing = m_editData.element != nullptr;
    if (!wasEditing && selection()->element()) {
        m_editData.element = selection()->element();
    }

    if (!m_editData.element) {
        return;
    }

    m_editData.key = event->key();
    m_editData.s = event->text();
    startEdit();
    int systemIndex = findSystemIndex(m_editData.element);
    int bracketIndex = findBracketIndex(m_editData.element);
    bool handled = m_editData.element->edit(m_editData) || (wasEditing && handleKeyPress(event));
    if (!wasEditing) {
        m_editData.element = nullptr;
    }
    if (handled) {
        event->accept();
        apply();
    } else {
        m_undoStack->rollbackChanges();
    }
    if (isTextEditingStarted()) {
        notifyAboutTextEditingChanged();
    } else {
        if (bracketIndex >= 0 && systemIndex < score()->systems().size()) {
            Ms::System* sys = score()->systems()[systemIndex];
            Ms::EngravingItem* bracket = sys->brackets()[bracketIndex];
            score()->select(bracket);
            notifyAboutSelectionChanged();
        }
    }
}

void NotationInteraction::endEditElement()
{
    if (!m_editData.element) {
        return;
    }
    if (isTextEditingStarted()) {
        endEditText();
        return;
    }
    if (isDragStarted()) {
        doEndDrag();
        rollback();
        m_dragData.reset();
    }
    if (m_editData.curGrip == Ms::Grip::NO_GRIP) {
        m_editData.element->endEdit(m_editData);
        m_editData.element = nullptr;
        resetAnchorLines();
    } else {
        m_editData.curGrip = Ms::Grip::NO_GRIP;
        updateAnchorLines();
    }
    notifyAboutNotationChanged();
}

void NotationInteraction::splitSelectedMeasure()
{
    EngravingItem* selectedElement = m_selection->element();
    if (!selectedElement) {
        return;
    }

    if (!selectedElement->isNote() && !selectedElement->isRest()) {
        return;
    }

    if (selectedElement->isNote()) {
        selectedElement = dynamic_cast<Note*>(selectedElement)->chord();
    }

    ChordRest* chordRest = dynamic_cast<ChordRest*>(selectedElement);

    startEdit();
    score()->cmdSplitMeasure(chordRest);
    apply();
}

void NotationInteraction::joinSelectedMeasures()
{
    if (!m_selection->isRange()) {
        return;
    }

    Measure* measureStart = score()->crMeasure(m_selection->range()->startMeasureIndex());
    Measure* measureEnd = score()->crMeasure(m_selection->range()->endMeasureIndex());

    startEdit();
    score()->cmdJoinMeasure(measureStart, measureEnd);
    apply();
}

void NotationInteraction::addBoxes(BoxType boxType, int count, int beforeBoxIndex)
{
    auto boxTypeToElementType = [](BoxType boxType) {
        switch (boxType) {
        case BoxType::Horizontal: return Ms::ElementType::HBOX;
        case BoxType::Vertical: return Ms::ElementType::VBOX;
        case BoxType::Text: return Ms::ElementType::TBOX;
        case BoxType::Measure: return Ms::ElementType::MEASURE;
        case BoxType::Unknown: return Ms::ElementType::INVALID;
        }

        return ElementType::INVALID;
    };

    Ms::ElementType elementType = boxTypeToElementType(boxType);
    Ms::MeasureBase* beforeBox = beforeBoxIndex >= 0 ? score()->measure(beforeBoxIndex) : nullptr;

    startEdit();
    for (int i = 0; i < count; ++i) {
        constexpr bool createEmptyMeasures = false;
        constexpr bool moveSignaturesClef = true;
        constexpr bool needDeselectAll = false;

        score()->insertMeasure(elementType, beforeBox, createEmptyMeasures, moveSignaturesClef, needDeselectAll);
    }
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::copySelection()
{
    if (!selection()->canCopy()) {
        return;
    }

    if (isTextEditingStarted()) {
        m_editData.element->editCopy(m_editData);
        Ms::TextEditData* ted = static_cast<Ms::TextEditData*>(m_editData.getData(m_editData.element));
        if (!ted->selectedText.isEmpty()) {
            QGuiApplication::clipboard()->setText(ted->selectedText, QClipboard::Clipboard);
        }
    } else {
        QMimeData* mimeData = selection()->mimeData();
        if (!mimeData) {
            return;
        }
        QApplication::clipboard()->setMimeData(mimeData);
    }
}

void NotationInteraction::copyLyrics()
{
    QString text = score()->extractLyrics();
    QApplication::clipboard()->setText(text);
}

void NotationInteraction::pasteSelection(const Fraction& scale)
{
    startEdit();

    if (isTextEditingStarted()) {
        #if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        QClipboard::Mode mode = QClipboard::Clipboard;
        #else
        QClipboard::Mode mode = QClipboard::Selection;
        #endif
        QString txt = QGuiApplication::clipboard()->text(mode);
        toTextBase(m_editData.element)->paste(m_editData, txt);
    } else {
        const QMimeData* mimeData = QApplication::clipboard()->mimeData();
        score()->cmdPaste(mimeData, nullptr, scale);
    }
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::swapSelection()
{
    if (!selection()->canCopy()) {
        return;
    }

    Ms::Selection& selection = score()->selection();
    QString mimeType = selection.mimeType();

    if (mimeType == Ms::mimeStaffListFormat) { // determine size of clipboard selection
        const QMimeData* mimeData = this->selection()->mimeData();
        QByteArray data = mimeData ? mimeData->data(Ms::mimeStaffListFormat) : QByteArray();
        Ms::XmlReader reader(data);
        reader.readNextStartElement();

        Fraction tickLen = Fraction(0, 1);
        int stavesCount = 0;

        if (reader.name() == "StaffList") {
            tickLen = Ms::Fraction::fromTicks(reader.intAttribute("len", 0));
            stavesCount = reader.intAttribute("staves", 0);
        }

        if (tickLen > Ms::Fraction(0, 1)) { // attempt to extend selection to match clipboard size
            Ms::Segment* segment = selection.startSegment();
            Ms::Fraction startTick = selection.tickStart() + tickLen;
            Ms::Segment* segmentAfter = score()->tick2leftSegment(startTick);

            int staffIndex = selection.staffStart() + stavesCount - 1;
            if (staffIndex >= score()->nstaves()) {
                staffIndex = score()->nstaves() - 1;
            }

            startTick = selection.tickStart();
            Ms::Fraction endTick = startTick + tickLen;
            selection.extendRangeSelection(segment, segmentAfter, staffIndex, startTick, endTick);
            selection.update();
        }
    }

    QByteArray currentSelectionBackup(selection.mimeData());
    pasteSelection();
    QMimeData* mimeData = new QMimeData();
    mimeData->setData(mimeType, currentSelectionBackup);
    QApplication::clipboard()->setMimeData(mimeData);
}

void NotationInteraction::deleteSelection()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    if (isTextEditingStarted()) {
        auto textBase = toTextBase(m_editData.element);
        if (!textBase->deleteSelectedText(m_editData)) {
            m_editData.key = Qt::Key_Backspace;
            m_editData.modifiers = {};
            textBase->edit(m_editData);
        }
    } else {
        score()->cmdDeleteSelection();
    }
    apply();

    resetGripEdit();

    notifyAboutSelectionChanged();
    notifyAboutNotationChanged();
}

void NotationInteraction::flipSelection()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdFlip();
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addTieToSelection()
{
    startEdit();
    score()->cmdToggleTie();
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addTiedNoteToChord()
{
    startEdit();
    score()->cmdAddTie(true);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addSlurToSelection()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    doAddSlur();
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addOttavaToSelection(OttavaType type)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdAddOttava(type);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addHairpinToSelection(HairpinType type)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->addHairpin(type);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addAccidentalToSelection(AccidentalType type)
{
    if (selection()->isNone()) {
        return;
    }

    Ms::EditData editData(&m_scoreCallbacks);

    startEdit();
    score()->toggleAccidental(type, editData);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::putRestToSelection()
{
    Ms::InputState& is = score()->inputState();
    if (!is.duration().isValid() || is.duration().isZero() || is.duration().isMeasure()) {
        is.setDuration(DurationType::V_QUARTER);
    }
    putRest(is.duration().type());
}

void NotationInteraction::putRest(DurationType duration)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdEnterRest(Duration(duration));
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addBracketsToSelection(BracketsType type)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();

    switch (type) {
    case BracketsType::Brackets:
        score()->cmdAddBracket();
        break;
    case BracketsType::Braces:
        score()->cmdAddBraces();
        break;
    case BracketsType::Parentheses:
        score()->cmdAddParentheses();
        break;
    }

    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::changeSelectedNotesArticulation(SymbolId articulationSymbolId)
{
    if (selection()->isNone()) {
        return;
    }

    std::vector<Ms::Note*> notes = score()->selection().noteList();

    auto updateMode = notesHaveActiculation(notes, articulationSymbolId)
                      ? Ms::ArticulationsUpdateMode::Remove : Ms::ArticulationsUpdateMode::Insert;

    std::set<Chord*> chords;
    for (Note* note: notes) {
        Chord* chord = note->chord();
        if (chords.find(chord) == chords.end()) {
            chords.insert(chord);
        }
    }

    startEdit();
    for (Chord* chord: chords) {
        chord->updateArticulations({ articulationSymbolId }, updateMode);
    }
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addGraceNotesToSelectedNotes(GraceNoteType type)
{
    if (selection()->isNone()) {
        return;
    }

    int denominator = 1;

    switch (type) {
    case GraceNoteType::GRACE4:
    case GraceNoteType::INVALID:
    case GraceNoteType::NORMAL:
        denominator = 1;
        break;
    case GraceNoteType::ACCIACCATURA:
    case GraceNoteType::APPOGGIATURA:
    case GraceNoteType::GRACE8_AFTER:
        denominator = 2;
        break;
    case GraceNoteType::GRACE16:
    case GraceNoteType::GRACE16_AFTER:
        denominator = 4;
        break;
    case GraceNoteType::GRACE32:
    case GraceNoteType::GRACE32_AFTER:
        denominator = 8;
        break;
    }

    startEdit();
    score()->cmdAddGrace(type, Ms::Constant::division / denominator);
    apply();

    notifyAboutNotationChanged();
}

bool NotationInteraction::canAddTupletToSelecredChordRests() const
{
    for (ChordRest* chordRest : score()->getSelectedChordRests()) {
        if (chordRest->isGrace()) {
            continue;
        }

        if (chordRest->durationType() < Ms::TDuration(Ms::DurationType::V_512TH)
            && chordRest->durationType() != Ms::TDuration(Ms::DurationType::V_MEASURE)) {
            return false;
        }
    }

    return true;
}

void NotationInteraction::addTupletToSelectedChordRests(const TupletOptions& options)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    for (ChordRest* chordRest : score()->getSelectedChordRests()) {
        if (!chordRest->isGrace()) {
            score()->addTuplet(chordRest, options.ratio, options.numberType, options.bracketType);
        }
    }

    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addBeamToSelectedChordRests(BeamMode mode)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdSetBeamMode(mode);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::increaseDecreaseDuration(int steps, bool stepByDots)
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdIncDecDuration(steps, stepByDots);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::toggleLayoutBreak(LayoutBreakType breakType)
{
    startEdit();
    score()->cmdToggleLayoutBreak(breakType);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::setBreaksSpawnInterval(BreaksSpawnIntervalType intervalType, int interval)
{
    interval = intervalType == BreaksSpawnIntervalType::MeasuresInterval ? interval : 0;
    bool afterEachSystem = intervalType == BreaksSpawnIntervalType::AfterEachSystem;

    startEdit();
    score()->addRemoveBreaks(interval, afterEachSystem);
    apply();

    notifyAboutNotationChanged();
}

bool NotationInteraction::transpose(const TransposeOptions& options)
{
    startEdit();

    bool ok = score()->transpose(options.mode, options.direction, options.key, options.interval,
                                 options.needTransposeKeys, options.needTransposeChordNames, options.needTransposeDoubleSharpsFlats);

    apply();

    notifyAboutNotationChanged();

    return ok;
}

void NotationInteraction::swapVoices(int voiceIndex1, int voiceIndex2)
{
    if (selection()->isNone()) {
        return;
    }

    if (voiceIndex1 == voiceIndex2) {
        return;
    }

    if (!isVoiceIndexValid(voiceIndex1) || !isVoiceIndexValid(voiceIndex2)) {
        return;
    }

    startEdit();
    score()->cmdExchangeVoice(voiceIndex1, voiceIndex2);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::addIntervalToSelectedNotes(int interval)
{
    if (!isNotesIntervalValid(interval)) {
        return;
    }

    std::vector<Note*> notes;

    if (score()->selection().isRange()) {
        for (const ChordRest* chordRest : score()->getSelectedChordRests()) {
            if (chordRest->isChord()) {
                const Chord* chord = toChord(chordRest);
                Note* note = interval > 0 ? chord->upNote() : chord->downNote();
                notes.push_back(note);
            }
        }
    } else {
        notes = score()->selection().noteList();
    }

    if (notes.empty()) {
        return;
    }

    startEdit();
    score()->addInterval(interval, notes);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addFret(int fretIndex)
{
    startEdit();
    score()->cmdAddFret(fretIndex);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::changeSelectedNotesVoice(int voiceIndex)
{
    if (selection()->isNone()) {
        return;
    }

    if (!isVoiceIndexValid(voiceIndex)) {
        return;
    }

    startEdit();
    score()->changeSelectedNotesVoice(voiceIndex);
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addAnchoredLineToSelectedNotes()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->addNoteLine();
    apply();

    notifyAboutSelectionChanged();
}

void NotationInteraction::addText(TextStyleType type)
{
    if (!scoreHasMeasure()) {
        LOGE() << "Need to create measure";
        return;
    }

    if (m_noteInput->isNoteInputMode()) {
        m_noteInput->endNoteInput();
    }

    startEdit();
    Ms::TextBase* textBox = score()->addText(type);
    apply();

    if (textBox) {
        doSelect({ textBox }, SelectType::SINGLE, 0);
        startEditText(textBox, PointF());
    }

    notifyAboutSelectionChanged();
}

void NotationInteraction::addFiguredBass()
{
    startEdit();
    Ms::FiguredBass* figuredBass = score()->addFiguredBass();

    if (figuredBass) {
        apply();
        startEditText(figuredBass, PointF());
        notifyAboutSelectionChanged();
    } else {
        rollback();
    }
}

void NotationInteraction::addStretch(qreal value)
{
    startEdit();
    score()->cmdAddStretch(value);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::addTimeSignature(Measure* measure, int staffIndex, TimeSignature* timeSignature)
{
    startEdit();
    score()->cmdAddTimeSig(measure, staffIndex, timeSignature, true);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::explodeSelectedStaff()
{
    if (!selection()->isRange()) {
        return;
    }

    startEdit();
    score()->cmdExplode();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::implodeSelectedStaff()
{
    if (!selection()->isRange()) {
        return;
    }

    startEdit();
    score()->cmdImplode();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::realizeSelectedChordSymbols()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdRealizeChordSymbols();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::removeSelectedRange()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdTimeDelete();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::removeEmptyTrailingMeasures()
{
    startEdit();
    score()->cmdRemoveEmptyTrailingMeasures();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::fillSelectionWithSlashes()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdSlashFill();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::replaceSelectedNotesWithSlashes()
{
    if (selection()->isNone()) {
        return;
    }

    startEdit();
    score()->cmdSlashRhythm();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::repeatSelection()
{
    const Ms::Selection& selection = score()->selection();
    if (score()->noteEntryMode() && selection.isSingle()) {
        EngravingItem* el = selection.element();
        if (el && el->type() == ElementType::NOTE && !score()->inputState().endOfScore()) {
            startEdit();
            Chord* c = toNote(el)->chord();
            for (Note* note : c->notes()) {
                Ms::NoteVal nval = note->noteVal();
                score()->addPitch(nval, note != c->notes()[0]);
            }
            apply();
            notifyAboutNotationChanged();
        }
        return;
    }
    if (!selection.isRange()) {
        ChordRest* cr = score()->getSelectedChordRest();
        if (!cr) {
            return;
        }
        score()->select(cr, SelectType::RANGE);
    }
    Ms::XmlReader xml(selection.mimeData());
    xml.setPasteMode(true);
    int dStaff = selection.staffStart();
    Ms::Segment* endSegment = selection.endSegment();

    if (endSegment && endSegment->segmentType() != Ms::SegmentType::ChordRest) {
        endSegment = endSegment->next1(Ms::SegmentType::ChordRest);
    }
    if (endSegment && endSegment->element(dStaff * Ms::VOICES)) {
        EngravingItem* e = endSegment->element(dStaff * Ms::VOICES);
        if (e) {
            startEdit();
            ChordRest* cr = toChordRest(e);
            score()->pasteStaff(xml, cr->segment(), cr->staffIdx());
            score()->endCmd();
            apply();
            notifyAboutNotationChanged();
        }
    }
}

void NotationInteraction::changeEnharmonicSpelling(bool both)
{
    startEdit();
    score()->changeEnharmonicSpelling(both);
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::spellPitches()
{
    startEdit();
    score()->spell();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::regroupNotesAndRests()
{
    startEdit();
    score()->cmdResetNoteAndRestGroupings();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::resequenceRehearsalMarks()
{
    startEdit();
    score()->cmdResequenceRehearsalMarks();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::unrollRepeats()
{
    if (!score()->masterScore()) {
        return;
    }

    startEdit();
    score()->masterScore()->unrollRepeats();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::resetStretch()
{
    startEdit();
    score()->resetUserStretch();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::resetTextStyleOverrides()
{
    startEdit();
    score()->cmdResetTextStyleOverrides();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::resetBeamMode()
{
    startEdit();
    score()->cmdResetBeamMode();
    apply();

    notifyAboutNotationChanged();
}

void NotationInteraction::resetShapesAndPosition()
{
    auto resetItem = [](EngravingItem* item) {
        item->reset();

        if (item->isSpanner()) {
            for (Ms::SpannerSegment* spannerSegment : toSpanner(item)->spannerSegments()) {
                spannerSegment->reset();
            }
        }
    };

    startEdit();

    Defer defer([this] {
        apply();
        notifyAboutNotationChanged();
    });

    if (selection()->element()) {
        resetItem(selection()->element());
        return;
    }

    for (EngravingItem* item : selection()->elements()) {
        resetItem(item);
    }
}

ScoreConfig NotationInteraction::scoreConfig() const
{
    ScoreConfig config;
    config.isShowInvisibleElements = score()->showInvisible();
    config.isShowUnprintableElements = score()->showUnprintable();
    config.isShowFrames = score()->showFrames();
    config.isShowPageMargins = score()->showPageborders();
    config.isMarkIrregularMeasures = score()->markIrregularMeasures();

    return config;
}

void NotationInteraction::setScoreConfig(ScoreConfig config)
{
    startEdit();
    score()->setShowInvisible(config.isShowInvisibleElements);
    score()->setShowUnprintable(config.isShowUnprintableElements);
    score()->setShowFrames(config.isShowFrames);
    score()->setShowPageborders(config.isShowPageMargins);
    score()->setMarkIrregularMeasures(config.isMarkIrregularMeasures);

    EngravingItem* selectedElement = selection()->element();
    if (selectedElement && !selectedElement->isInteractionAvailable()) {
        clearSelection();
    }

    apply();

    notifyAboutNotationChanged();
}

bool NotationInteraction::needEndTextEditing(const std::vector<EngravingItem*>& newSelectedElements) const
{
    if (!isTextEditingStarted()) {
        return false;
    }

    if (newSelectedElements.empty()) {
        return false;
    }

    if (newSelectedElements.size() > 1) {
        return true;
    }

    return newSelectedElements.front() != m_editData.element;
}

void NotationInteraction::updateGripEdit()
{
    const auto& elements = score()->selection().elements();

    if (elements.isEmpty() || elements.size() > 1) {
        resetGripEdit();
        return;
    }

    EngravingItem* element = elements.front();
    if (element->gripsCount() <= 0) {
        resetGripEdit();
        return;
    }
    m_editData.grips = element->gripsCount();
    m_editData.curGrip = Ms::Grip::NO_GRIP;
    bool editingElement = m_editData.element != nullptr;
    m_editData.element = element;
    element->startEdit(m_editData);
    updateAnchorLines();
    if (!editingElement) {
        m_editData.element = nullptr;
    }
}

void NotationInteraction::resetGripEdit()
{
    m_editData.grips = 0;
    m_editData.curGrip = Ms::Grip::NO_GRIP;
    m_editData.grip.clear();

    resetAnchorLines();
}

void NotationInteraction::navigateToLyrics(bool back, bool moveOnly, bool end)
{
    if (!m_editData.element || !m_editData.element->isLyrics()) {
        qWarning("nextLyric called with invalid current element");
        return;
    }
    Ms::Lyrics* lyrics = toLyrics(m_editData.element);
    int track = lyrics->track();
    Ms::Segment* segment = lyrics->segment();
    int verse = lyrics->no();
    Ms::PlacementV placement = lyrics->placement();
    Ms::PropertyFlags pFlags = lyrics->propertyFlags(Ms::Pid::PLACEMENT);

    Ms::Segment* nextSegment = segment;
    if (back) {
        // search prev chord
        while ((nextSegment = nextSegment->prev1(Ms::SegmentType::ChordRest))) {
            EngravingItem* el = nextSegment->element(track);
            if (el && el->isChord()) {
                break;
            }
        }
    } else {
        // search next chord
        while ((nextSegment = nextSegment->next1(Ms::SegmentType::ChordRest))) {
            EngravingItem* el = nextSegment->element(track);
            if (el && el->isChord()) {
                break;
            }
        }
    }
    if (nextSegment == 0) {
        return;
    }

    endEditText();

    // look for the lyrics we are moving from; may be the current lyrics or a previous one
    // if we are skipping several chords with spaces
    Ms::Lyrics* fromLyrics = 0;
    if (!back) {
        while (segment) {
            ChordRest* cr = toChordRest(segment->element(track));
            if (cr) {
                fromLyrics = cr->lyrics(verse, placement);
                if (fromLyrics) {
                    break;
                }
            }
            segment = segment->prev1(Ms::SegmentType::ChordRest);
        }
    }

    ChordRest* cr = toChordRest(nextSegment->element(track));
    if (!cr) {
        qDebug("no next lyrics list: %s", nextSegment->element(track)->name());
        return;
    }
    Ms::Lyrics* nextLyrics = cr->lyrics(verse, placement);

    bool newLyrics = false;
    if (!nextLyrics) {
        nextLyrics = Factory::createLyrics(cr);
        nextLyrics->setTrack(track);
        cr = toChordRest(nextSegment->element(track));
        nextLyrics->setParent(cr);
        nextLyrics->setNo(verse);
        nextLyrics->setPlacement(placement);
        nextLyrics->setPropertyFlags(Ms::Pid::PLACEMENT, pFlags);
        nextLyrics->setSyllabic(Ms::Lyrics::Syllabic::SINGLE);
        newLyrics = true;
    }

    score()->startCmd();
    if (fromLyrics && !moveOnly) {
        switch (nextLyrics->syllabic()) {
        // as we arrived at nextLyrics by a [Space], it can be the beginning
        // of a multi-syllable, but cannot have syllabic dashes before
        case Ms::Lyrics::Syllabic::SINGLE:
        case Ms::Lyrics::Syllabic::BEGIN:
            break;
        case Ms::Lyrics::Syllabic::END:
            nextLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::SINGLE));
            break;
        case Ms::Lyrics::Syllabic::MIDDLE:
            nextLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::BEGIN));
            break;
        }
        // as we moved away from fromLyrics by a [Space], it can be
        // the end of a multi-syllable, but cannot have syllabic dashes after
        switch (fromLyrics->syllabic()) {
        case Ms::Lyrics::Syllabic::SINGLE:
        case Ms::Lyrics::Syllabic::END:
            break;
        case Ms::Lyrics::Syllabic::BEGIN:
            fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::SINGLE));
            break;
        case Ms::Lyrics::Syllabic::MIDDLE:
            fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::END));
            break;
        }
        // for the same reason, it cannot have a melisma
        fromLyrics->undoChangeProperty(Ms::Pid::LYRIC_TICKS, 0);
    }

    if (newLyrics) {
        score()->undoAddElement(nextLyrics);
    }
    score()->endCmd();
    score()->select(nextLyrics, SelectType::SINGLE, 0);
    score()->setLayoutAll();

    startEditText(nextLyrics, PointF());

    Ms::TextCursor* cursor = nextLyrics->cursor();
    if (end) {
        nextLyrics->selectAll(cursor);
    } else {
        cursor->movePosition(Ms::TextCursor::MoveOperation::End, Ms::TextCursor::MoveMode::MoveAnchor);
        cursor->movePosition(Ms::TextCursor::MoveOperation::Start, Ms::TextCursor::MoveMode::KeepAnchor);
    }
}

void NotationInteraction::navigateToLyrics(MoveDirection direction)
{
    navigateToLyrics(direction == MoveDirection::Left, true, false);
}

void NotationInteraction::nagivateToNextSyllable()
{
    if (!m_editData.element || !m_editData.element->isLyrics()) {
        qWarning("nextSyllable called with invalid current element");
        return;
    }
    Ms::Lyrics* lyrics = toLyrics(m_editData.element);
    int track = lyrics->track();
    Ms::Segment* segment = lyrics->segment();
    int verse = lyrics->no();
    Ms::PlacementV placement = lyrics->placement();
    Ms::PropertyFlags pFlags = lyrics->propertyFlags(Ms::Pid::PLACEMENT);

    // search next chord
    Ms::Segment* nextSegment = segment;
    while ((nextSegment = nextSegment->next1(Ms::SegmentType::ChordRest))) {
        EngravingItem* el = nextSegment->element(track);
        if (el && el->isChord()) {
            break;
        }
    }
    if (nextSegment == 0) {
        return;
    }

    // look for the lyrics we are moving from; may be the current lyrics or a previous one
    // we are extending with several dashes
    Ms::Lyrics* fromLyrics = 0;
    while (segment) {
        ChordRest* cr = toChordRest(segment->element(track));
        if (!cr) {
            segment = segment->prev1(Ms::SegmentType::ChordRest);
            continue;
        }
        fromLyrics = cr->lyrics(verse, placement);
        if (fromLyrics) {
            break;
        }
        segment = segment->prev1(Ms::SegmentType::ChordRest);
    }

    endEditText();

    score()->startCmd();
    ChordRest* cr = toChordRest(nextSegment->element(track));
    Ms::Lyrics* toLyrics = cr->lyrics(verse, placement);
    bool newLyrics = (toLyrics == 0);
    if (!toLyrics) {
        toLyrics = Factory::createLyrics(cr);
        toLyrics->setTrack(track);
        toLyrics->setParent(cr);
        toLyrics->setNo(verse);
        toLyrics->setPlacement(placement);
        toLyrics->setPropertyFlags(Ms::Pid::PLACEMENT, pFlags);
        toLyrics->setSyllabic(Ms::Lyrics::Syllabic::END);
    } else {
        // as we arrived at toLyrics by a dash, it cannot be initial or isolated
        if (toLyrics->syllabic() == Ms::Lyrics::Syllabic::BEGIN) {
            toLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::MIDDLE));
        } else if (toLyrics->syllabic() == Ms::Lyrics::Syllabic::SINGLE) {
            toLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::END));
        }
    }

    if (fromLyrics) {
        // as we moved away from fromLyrics by a dash,
        // it can have syll. dashes before and after but cannot be isolated or terminal
        switch (fromLyrics->syllabic()) {
        case Ms::Lyrics::Syllabic::BEGIN:
        case Ms::Lyrics::Syllabic::MIDDLE:
            break;
        case Ms::Lyrics::Syllabic::SINGLE:
            fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::BEGIN));
            break;
        case Ms::Lyrics::Syllabic::END:
            fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::MIDDLE));
            break;
        }
        // for the same reason, it cannot have a melisma
        fromLyrics->undoChangeProperty(Ms::Pid::LYRIC_TICKS, Fraction(0, 1));
    }

    if (newLyrics) {
        score()->undoAddElement(toLyrics);
    }

    score()->endCmd();
    score()->select(toLyrics, SelectType::SINGLE, 0);
    score()->setLayoutAll();

    startEditText(toLyrics, PointF());

    toLyrics->selectAll(toLyrics->cursor());
}

void NotationInteraction::navigateToLyricsVerse(MoveDirection direction)
{
    if (!m_editData.element || !m_editData.element->isLyrics()) {
        qWarning("nextLyricVerse called with invalid current element");
        return;
    }
    Ms::Lyrics* lyrics = toLyrics(m_editData.element);
    int track = lyrics->track();
    ChordRest* cr = lyrics->chordRest();
    int verse = lyrics->no();
    Ms::PlacementV placement = lyrics->placement();
    Ms::PropertyFlags pFlags = lyrics->propertyFlags(Ms::Pid::PLACEMENT);

    if (direction == MoveDirection::Up) {
        if (verse == 0) {
            return;
        }
        --verse;
    } else {
        ++verse;
        if (verse > cr->lastVerse(placement)) {
            return;
        }
    }

    endEditText();

    lyrics = cr->lyrics(verse, placement);
    if (!lyrics) {
        lyrics = Factory::createLyrics(cr);
        lyrics->setTrack(track);
        lyrics->setParent(cr);
        lyrics->setNo(verse);
        lyrics->setPlacement(placement);
        lyrics->setPropertyFlags(Ms::Pid::PLACEMENT, pFlags);
        score()->startCmd();
        score()->undoAddElement(lyrics);
        score()->endCmd();
    }

    score()->select(lyrics, SelectType::SINGLE, 0);
    startEditText(lyrics, PointF());

    lyrics = toLyrics(m_editData.element);

    score()->setLayoutAll();
    score()->update();

    lyrics->selectAll(lyrics->cursor());
}

//! NOTE: Copied from ScoreView::harmonyBeatsTab
void NotationInteraction::navigateToHarmonyInNearBeat(MoveDirection direction, bool noterest)
{
    Ms::Harmony* harmony = editedHarmony();
    Ms::Segment* segment = harmony ? toSegment(harmony->parent()) : nullptr;
    if (!segment) {
        qDebug("no segment");
        return;
    }

    Measure* measure = segment->measure();
    Fraction tick = segment->tick();
    int track = harmony->track();
    bool backDirection = direction == MoveDirection::Left;

    if (backDirection && tick == measure->tick()) {
        // previous bar, if any
        measure = measure->prevMeasure();
        if (!measure) {
            qDebug("no previous measure");
            return;
        }
    }

    Fraction f = measure->ticks();
    int ticksPerBeat   = f.ticks()
                         / ((f.numerator() > 3 && (f.numerator() % 3) == 0 && f.denominator() > 4) ? f.numerator() / 3 : f.numerator());
    Fraction tickInBar = tick - measure->tick();
    Fraction newTick   = measure->tick()
                         + Fraction::fromTicks((
                                                   (tickInBar.ticks() + (backDirection ? -1 : ticksPerBeat)) / ticksPerBeat
                                                   )
                                               * ticksPerBeat);

    bool needAddSegment = false;

    // look for next/prev beat, note, rest or chord
    for (;;) {
        segment = backDirection ? segment->prev1(Ms::SegmentType::ChordRest) : segment->next1(Ms::SegmentType::ChordRest);

        if (!segment || (backDirection ? (segment->tick() < newTick) : (segment->tick() > newTick))) {
            // no segment or moved past the beat - create new segment
            if (!backDirection && newTick >= measure->tick() + f) {
                // next bar, if any
                measure = measure->nextMeasure();
                if (!measure) {
                    qDebug("no next measure");
                    return;
                }
            }

            segment = Factory::createSegment(measure, Ms::SegmentType::ChordRest, newTick - measure->tick());
            if (!segment) {
                qDebug("no prev segment");
                return;
            }
            needAddSegment = true;
            break;
        }

        if (segment->tick() == newTick) {
            break;
        }

        if (noterest) {
            int minTrack = (track / Ms::VOICES) * Ms::VOICES;
            int maxTrack = minTrack + (Ms::VOICES - 1);
            if (segment->hasAnnotationOrElement(ElementType::HARMONY, minTrack, maxTrack)) {
                break;
            }
        }
    }

    startEdit();

    if (needAddSegment) {
        score()->undoAddElement(segment);
    }

    Ms::Harmony* nextHarmony = findHarmonyInSegment(segment, track, harmony->textStyleType());
    if (!nextHarmony) {
        nextHarmony = createHarmony(segment, track, harmony->harmonyType());
        score()->undoAddElement(nextHarmony);
    }

    apply();
    startEditText(nextHarmony);
}

//! NOTE: Copied from ScoreView::harmonyTab
void NotationInteraction::navigateToHarmonyInNearMeasure(MoveDirection direction)
{
    Ms::Harmony* harmony = editedHarmony();
    Ms::Segment* segment = harmony ? toSegment(harmony->parent()) : nullptr;
    if (!segment) {
        qDebug("harmonyTicksTab: no segment");
        return;
    }

    // moving to next/prev measure
    Measure* measure = segment->measure();
    if (measure) {
        if (direction == MoveDirection::Left) {
            measure = measure->prevMeasure();
        } else {
            measure = measure->nextMeasure();
        }
    }

    if (!measure) {
        qDebug("no prev/next measure");
        return;
    }

    segment = measure->findSegment(Ms::SegmentType::ChordRest, measure->tick());
    if (!segment) {
        qDebug("no ChordRest segment as measure");
        return;
    }

    int track = harmony->track();

    Ms::Harmony* nextHarmony = findHarmonyInSegment(segment, track, harmony->textStyleType());
    if (!nextHarmony) {
        nextHarmony = createHarmony(segment, track, harmony->harmonyType());

        startEdit();
        score()->undoAddElement(nextHarmony);
        apply();
    }

    startEditText(nextHarmony);
}

//! NOTE: Copied from ScoreView::harmonyBeatsTab
void NotationInteraction::navigateToHarmony(const Fraction& ticks)
{
    Ms::Harmony* harmony = editedHarmony();
    Ms::Segment* segment = harmony ? toSegment(harmony->parent()) : nullptr;
    if (!segment) {
        qDebug("no segment");
        return;
    }

    Measure* measure = segment->measure();

    Fraction newTick   = segment->tick() + ticks;

    // find the measure containing the target tick
    while (newTick >= measure->tick() + measure->ticks()) {
        measure = measure->nextMeasure();
        if (!measure) {
            qDebug("no next measure");
            return;
        }
    }

    // look for a segment at this tick; if none, create one
    while (segment && segment->tick() < newTick) {
        segment = segment->next1(Ms::SegmentType::ChordRest);
    }

    startEdit();

    if (!segment || segment->tick() > newTick) {      // no ChordRest segment at this tick
        segment = Factory::createSegment(measure, Ms::SegmentType::ChordRest, newTick - measure->tick());
        score()->undoAddElement(segment);
    }

    int track = harmony->track();

    Ms::Harmony* nextHarmony = findHarmonyInSegment(segment, track, harmony->textStyleType());
    if (!nextHarmony) {
        nextHarmony = createHarmony(segment, track, harmony->harmonyType());
        score()->undoAddElement(nextHarmony);
    }

    apply();
    startEditText(nextHarmony);
}

//! NOTE: Copied from ScoreView::figuredBassTab
void NotationInteraction::navigateToFiguredBassInNearBeat(MoveDirection direction)
{
    Ms::FiguredBass* fb = Ms::toFiguredBass(m_editData.element);
    Ms::Segment* segm = fb->segment();
    int track = fb->track();
    bool backDirection = direction == MoveDirection::Left;

    if (!segm) {
        qDebug("figuredBassTab: no segment");
        return;
    }

    // search next chord segment in same staff
    Ms::Segment* nextSegm = backDirection ? segm->prev1(Ms::SegmentType::ChordRest) : segm->next1(Ms::SegmentType::ChordRest);
    int minTrack = (track / Ms::VOICES) * Ms::VOICES;
    int maxTrack = minTrack + (Ms::VOICES - 1);

    while (nextSegm) { // look for a ChordRest in the compatible track range
        if (nextSegm->hasAnnotationOrElement(ElementType::FIGURED_BASS, minTrack, maxTrack)) {
            break;
        }
        nextSegm = backDirection ? nextSegm->prev1(Ms::SegmentType::ChordRest) : nextSegm->next1(Ms::SegmentType::ChordRest);
    }

    if (!nextSegm) {
        qDebug("figuredBassTab: no prev/next segment");
        return;
    }

    bool bNew = false;
    // add a (new) FB element, using chord duration as default duration
    Ms::FiguredBass* fbNew = Ms::FiguredBass::addFiguredBassToSegment(nextSegm, track, Fraction(0, 1), &bNew);
    if (bNew) {
        startEdit();
        score()->undoAddElement(fbNew);
        apply();
    }

    startEditText(fbNew);
}

//! NOTE: Copied from ScoreView::figuredBassTab
void NotationInteraction::navigateToFiguredBassInNearMeasure(MoveDirection direction)
{
    Ms::FiguredBass* fb = Ms::toFiguredBass(m_editData.element);
    Ms::Segment* segm = fb->segment();

    if (!segm) {
        qDebug("figuredBassTab: no segment");
        return;
    }

    // if moving to next/prev measure
    Measure* meas = segm->measure();
    if (meas) {
        if (direction == MoveDirection::Left) {
            meas = meas->prevMeasure();
        } else {
            meas = meas->nextMeasure();
        }
    }
    if (!meas) {
        qDebug("figuredBassTab: no prev/next measure");
        return;
    }
    // find initial ChordRest segment
    Ms::Segment* nextSegm = meas->findSegment(Ms::SegmentType::ChordRest, meas->tick());
    if (!nextSegm) {
        qDebug("figuredBassTab: no ChordRest segment at measure");
        return;
    }

    bool bNew = false;
    // add a (new) FB element, using chord duration as default duration
    Ms::FiguredBass* fbNew = Ms::FiguredBass::addFiguredBassToSegment(nextSegm, fb->track(), Fraction(0, 1), &bNew);
    if (bNew) {
        startEdit();
        score()->undoAddElement(fbNew);
        apply();
    }

    startEditText(fbNew);
}

//! NOTE: Copied from ScoreView::figuredBassTicksTab
void NotationInteraction::navigateToFiguredBass(const Fraction& ticks)
{
    Ms::FiguredBass* fb = Ms::toFiguredBass(m_editData.element);
    int track = fb->track();
    Ms::Segment* segm = fb->segment();
    if (!segm) {
        qDebug("figuredBassTicksTab: no segment");
        return;
    }
    Measure* measure = segm->measure();

    Fraction nextSegTick   = segm->tick() + ticks;

    // find the measure containing the target tick
    while (nextSegTick >= measure->tick() + measure->ticks()) {
        measure = measure->nextMeasure();
        if (!measure) {
            qDebug("figuredBassTicksTab: no next measure");
            return;
        }
    }

    // look for a segment at this tick; if none, create one
    Ms::Segment* nextSegm = segm;
    while (nextSegm && nextSegm->tick() < nextSegTick) {
        nextSegm = nextSegm->next1(Ms::SegmentType::ChordRest);
    }

    bool needAddSegment = false;

    if (!nextSegm || nextSegm->tick() > nextSegTick) {      // no ChordRest segm at this tick
        nextSegm = Factory::createSegment(measure, Ms::SegmentType::ChordRest, nextSegTick - measure->tick());
        if (!nextSegm) {
            qDebug("figuredBassTicksTab: no next segment");
            return;
        }
        needAddSegment = true;
    }

    startEdit();

    if (needAddSegment) {
        score()->undoAddElement(nextSegm);
    }

    bool bNew = false;
    Ms::FiguredBass* fbNew = Ms::FiguredBass::addFiguredBassToSegment(nextSegm, track, ticks, &bNew);
    if (bNew) {
        score()->undoAddElement(fbNew);
    }

    apply();
    startEditText(fbNew);
}

void NotationInteraction::addMelisma()
{
    if (!m_editData.element || !m_editData.element->isLyrics()) {
        qWarning("addMelisma called with invalid current element");
        return;
    }
    Ms::Lyrics* lyrics = toLyrics(m_editData.element);
    int track = lyrics->track();
    Ms::Segment* segment = lyrics->segment();
    int verse = lyrics->no();
    Ms::PlacementV placement = lyrics->placement();
    Ms::PropertyFlags pFlags = lyrics->propertyFlags(Ms::Pid::PLACEMENT);
    Fraction endTick = segment->tick(); // a previous melisma cannot extend beyond this point

    endEditText();

    // search next chord
    Ms::Segment* nextSegment = segment;
    while ((nextSegment = nextSegment->next1(Ms::SegmentType::ChordRest))) {
        EngravingItem* el = nextSegment->element(track);
        if (el && el->isChord()) {
            break;
        }
    }

    // look for the lyrics we are moving from; may be the current lyrics or a previous one
    // we are extending with several underscores
    Ms::Lyrics* fromLyrics = 0;
    while (segment) {
        ChordRest* cr = toChordRest(segment->element(track));
        if (cr) {
            fromLyrics = cr->lyrics(verse, placement);
            if (fromLyrics) {
                break;
            }
        }
        segment = segment->prev1(Ms::SegmentType::ChordRest);
        // if the segment has a rest in this track, stop going back
        EngravingItem* e = segment ? segment->element(track) : 0;
        if (e && !e->isChord()) {
            break;
        }
    }

    // one-chord melisma?
    // if still at melisma initial chord and there is a valid next chord (if not,
    // there will be no melisma anyway), set a temporary melisma duration
    if (fromLyrics == lyrics && nextSegment) {
        score()->startCmd();
        lyrics->undoChangeProperty(Ms::Pid::LYRIC_TICKS, Fraction::fromTicks(Ms::Lyrics::TEMP_MELISMA_TICKS));
        score()->setLayoutAll();
        score()->endCmd();
    }

    if (nextSegment == 0) {
        score()->startCmd();
        if (fromLyrics) {
            switch (fromLyrics->syllabic()) {
            case Ms::Lyrics::Syllabic::SINGLE:
            case Ms::Lyrics::Syllabic::END:
                break;
            default:
                fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::END));
                break;
            }
            if (fromLyrics->segment()->tick() < endTick) {
                fromLyrics->undoChangeProperty(Ms::Pid::LYRIC_TICKS, endTick - fromLyrics->segment()->tick());
            }
        }

        if (fromLyrics) {
            score()->select(fromLyrics, SelectType::SINGLE, 0);
        }
        score()->setLayoutAll();
        score()->endCmd();
        return;
    }

    // if a place for a new lyrics has been found, create a lyrics there

    score()->startCmd();
    ChordRest* cr = toChordRest(nextSegment->element(track));
    Ms::Lyrics* toLyrics = cr->lyrics(verse, placement);
    bool newLyrics = (toLyrics == 0);
    if (!toLyrics) {
        toLyrics = Factory::createLyrics(cr);
        toLyrics->setTrack(track);
        toLyrics->setParent(cr);
        toLyrics->setNo(verse);
        toLyrics->setPlacement(placement);
        toLyrics->setPropertyFlags(Ms::Pid::PLACEMENT, pFlags);
        toLyrics->setSyllabic(Ms::Lyrics::Syllabic::SINGLE);
    }
    // as we arrived at toLyrics by an underscore, it cannot have syllabic dashes before
    else if (toLyrics->syllabic() == Ms::Lyrics::Syllabic::MIDDLE) {
        toLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::BEGIN));
    } else if (toLyrics->syllabic() == Ms::Lyrics::Syllabic::END) {
        toLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::SINGLE));
    }
    if (fromLyrics) {
        // as we moved away from fromLyrics by an underscore,
        // it can be isolated or terminal but cannot have dashes after
        switch (fromLyrics->syllabic()) {
        case Ms::Lyrics::Syllabic::SINGLE:
        case Ms::Lyrics::Syllabic::END:
            break;
        default:
            fromLyrics->undoChangeProperty(Ms::Pid::SYLLABIC, int(Ms::Lyrics::Syllabic::END));
            break;
        }
        // for the same reason, if it has a melisma, this cannot extend beyond toLyrics
        if (fromLyrics->segment()->tick() < endTick) {
            fromLyrics->undoChangeProperty(Ms::Pid::LYRIC_TICKS, endTick - fromLyrics->segment()->tick());
        }
    }
    if (newLyrics) {
        score()->undoAddElement(toLyrics);
    }
    score()->endCmd();

    score()->select(toLyrics, SelectType::SINGLE, 0);
    startEditText(toLyrics, PointF());

    toLyrics->selectAll(toLyrics->cursor());
}

void NotationInteraction::addLyricsVerse()
{
    if (!m_editData.element || !m_editData.element->isLyrics()) {
        qWarning("nextLyricVerse called with invalid current element");
        return;
    }
    Ms::Lyrics* lyrics = toLyrics(m_editData.element);

    endEditText();

    score()->startCmd();
    int newVerse;
    newVerse = lyrics->no() + 1;

    Ms::Lyrics* oldLyrics = lyrics;
    lyrics = Factory::createLyrics(oldLyrics->chordRest());
    lyrics->setTrack(oldLyrics->track());
    lyrics->setParent(oldLyrics->chordRest());
    lyrics->setPlacement(oldLyrics->placement());
    lyrics->setPropertyFlags(Ms::Pid::PLACEMENT, oldLyrics->propertyFlags(Ms::Pid::PLACEMENT));
    lyrics->setNo(newVerse);

    score()->undoAddElement(lyrics);
    score()->endCmd();

    score()->select(lyrics, SelectType::SINGLE, 0);
    startEditText(lyrics, PointF());
}

Ms::Harmony* NotationInteraction::editedHarmony() const
{
    Harmony* harmony = static_cast<Harmony*>(m_editData.element);
    if (!harmony) {
        return nullptr;
    }

    if (!harmony->parent() || !harmony->parent()->isSegment()) {
        qDebug("no segment parent");
        return nullptr;
    }

    return harmony;
}

Ms::Harmony* NotationInteraction::findHarmonyInSegment(const Ms::Segment* segment, int track, Ms::TextStyleType textStyleType) const
{
    for (Ms::EngravingItem* e : segment->annotations()) {
        if (e->isHarmony() && e->track() == track && toHarmony(e)->textStyleType() == textStyleType) {
            return toHarmony(e);
        }
    }

    return nullptr;
}

Ms::Harmony* NotationInteraction::createHarmony(Ms::Segment* segment, int track, Ms::HarmonyType type) const
{
    Ms::Harmony* harmony = Factory::createHarmony(score()->dummy()->segment());
    harmony->setScore(score());
    harmony->setParent(segment);
    harmony->setTrack(track);
    harmony->setHarmonyType(type);

    return harmony;
}

void NotationInteraction::startEditText(Ms::TextBase* text)
{
    doEndTextEdit();
    select({ text }, SelectType::SINGLE);
    startEditText(text, PointF());
    text->cursor()->moveCursorToEnd();
}

void NotationInteraction::doEndTextEdit()
{
    if (m_editData.element) {
        m_editData.element->endEdit(m_editData);
        m_editData.element = nullptr;
    }

    m_editData.clearData();
}

bool NotationInteraction::needEndTextEdit() const
{
    if (isTextEditingStarted()) {
        const Ms::TextBase* text = Ms::toTextBase(m_editData.element);
        return !text || !text->cursor()->editing();
    }

    return false;
}

void NotationInteraction::toggleFontStyle(Ms::FontStyle style)
{
    if (!m_editData.element || !m_editData.element->isTextBase()) {
        qWarning("toggleFontStyle called with invalid current element");
        return;
    }
    Ms::TextBase* text = toTextBase(m_editData.element);
    int currentStyle = text->getProperty(Ms::Pid::FONT_STYLE).toInt();
    score()->startCmd();
    text->undoChangeProperty(Ms::Pid::FONT_STYLE, PropertyValue::fromValue(
                                 currentStyle ^ static_cast<int>(style)), Ms::PropertyFlags::UNSTYLED);
    score()->endCmd();
    notifyAboutTextEditingChanged();
}

void NotationInteraction::toggleBold()
{
    toggleFontStyle(Ms::FontStyle::Bold);
}

void NotationInteraction::toggleItalic()
{
    toggleFontStyle(Ms::FontStyle::Italic);
}

void NotationInteraction::toggleUnderline()
{
    toggleFontStyle(Ms::FontStyle::Underline);
}

void NotationInteraction::toggleStrike()
{
    toggleFontStyle(Ms::FontStyle::Strike);
}

template<typename P>
void NotationInteraction::execute(void (Ms::Score::* function)(P), P param)
{
    startEdit();
    (score()->*function)(param);
    apply();
    notifyAboutNotationChanged();
}

void NotationInteraction::toggleArticulation(Ms::SymId symId)
{
    execute(&Ms::Score::toggleArticulation, symId);
}

void NotationInteraction::toggleAutoplace(bool all)
{
    execute(&Ms::Score::cmdToggleAutoplace, all);
}

void NotationInteraction::insertClef(Ms::ClefType clef)
{
    execute(&Ms::Score::cmdInsertClef, clef);
}

void NotationInteraction::changeAccidental(Ms::AccidentalType accidental)
{
    execute(&Ms::Score::changeAccidental, accidental);
}

void NotationInteraction::transposeSemitone(int steps)
{
    execute(&Ms::Score::transposeSemitone, steps);
}

void NotationInteraction::transposeDiatonicAlterations(Ms::TransposeDirection direction)
{
    execute(&Ms::Score::transposeDiatonicAlterations, direction);
}

void NotationInteraction::toggleGlobalOrLocalInsert()
{
    score()->inputState().setInsertMode(!score()->inputState().insertMode());
}

void NotationInteraction::getLocation()
{
    auto* e = score()->selection().element();
    if (!e) {
        // no current selection - restore lost selection
        e = score()->selection().currentCR();
        if (e && e->isChord()) {
            e = toChord(e)->upNote();
        }
    }
    if (!e) {
        e = score()->firstElement(false);
    }
    if (e) {
        if (e->type() == ElementType::NOTE || e->type() == ElementType::HARMONY) {
            score()->setPlayNote(true);
        }
        select({ e }, SelectType::SINGLE);
    }
}

void NotationInteraction::execute(void (Ms::Score::* function)())
{
    startEdit();
    (score()->*function)();
    apply();
    notifyAboutNotationChanged();
}
