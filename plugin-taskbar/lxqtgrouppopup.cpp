/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXQt - a lightweight, Qt based, desktop toolset
 * https://lxqt.org
 *
 * Copyright: 2011 Razor team
 *            2014 LXQt team
 * Authors:
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *   Maciej Płaza <plaza.maciej@gmail.com>
 *   Kuzma Shapran <kuzma.shapran@gmail.com>
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include "lxqtgrouppopup.h"
#include <QEnterEvent>
#include <QDrag>
#include <QMimeData>
#include <QLayout>
#include <QPainter>
#include <QStyleOption>
#include <QDebug>
#include <unistd.h>

/************************************************
    this class is just a container of window buttons
    the main purpose is showing window buttons in
    vertical layout and drag&drop feature inside
    group
 ************************************************/
LXQtGroupPopup::LXQtGroupPopup(LXQtTaskGroup *group):
    QFrame(mGroup = group),
    mLayout(this)
{
    Q_ASSERT(mGroup);
    setAcceptDrops(true);
    setWindowFlags(Qt::FramelessWindowHint | Qt::ToolTip);
    setAttribute(Qt::WA_AlwaysShowToolTips);
    setAttribute(Qt::WA_TranslucentBackground);

    mLayout.setSpacing(3);
    mLayout.setMargin(3);
}

LXQtGroupPopup::~LXQtGroupPopup() = default;

void LXQtGroupPopup::dropEvent(QDropEvent *event)
{
    qlonglong temp;
    QDataStream stream(event->mimeData()->data(LXQtTaskButton::mimeDataFormat()));
    stream >> temp;
    WId window = (WId) temp;

    LXQtTaskButton *button = nullptr;
    int oldIndex(0);
    // get current position of the button being dragged
    for (int i = 0; i < layout()->count(); i++)
    {
        LXQtTaskButton *b = qobject_cast<LXQtTaskButton*>(layout()->itemAt(i)->widget());
        if (b && b->windowId() == window)
        {
            button = b;
            oldIndex = i;
            break;
        }
    }

    if (button == nullptr)
        return;

    int newIndex = -1;
    // find the new position to place it in
    for (int i = 0; i < oldIndex && newIndex == -1; i++)
    {
        QWidget *w = layout()->itemAt(i)->widget();
        if (w && w->pos().y() + w->height() / 2 > event->pos().y())
            newIndex = i;
    }
    const int size = layout()->count();
    for (int i = size - 1; i > oldIndex && newIndex == -1; i--)
    {
        QWidget *w = layout()->itemAt(i)->widget();
        if (w && w->pos().y() + w->height() / 2 < event->pos().y())
            newIndex = i;
    }

    if (newIndex == -1 || newIndex == oldIndex)
        return;

    QVBoxLayout * l = qobject_cast<QVBoxLayout *>(layout());
    l->takeAt(oldIndex);
    l->insertWidget(newIndex, button);
    l->invalidate();

}

void LXQtGroupPopup::dragEnterEvent(QDragEnterEvent *event)
{
    event->accept();
    QWidget::dragEnterEvent(event);
}

void LXQtGroupPopup::dragLeaveEvent(QDragLeaveEvent *event)
{
    hide(false/*not fast*/);
    QFrame::dragLeaveEvent(event);
}

/************************************************
 *
 ************************************************/
void LXQtGroupPopup::leaveEvent(QEvent * /*event*/)
{
    if (!mCloseTimerId)
        mCloseTimerId = startTimer(400);
}

/************************************************
 *
 ************************************************/
void LXQtGroupPopup::enterEvent(QEvent * /*event*/)
{
    killTimer(mCloseTimerId);
    mCloseTimerId = 0;
}

void LXQtGroupPopup::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void LXQtGroupPopup::hide(bool fast)
{
    if (fast)
        close();
    else
        leaveEvent(nullptr);
}

void LXQtGroupPopup::show()
{
    enterEvent(nullptr);
    QFrame::show();
}

void LXQtGroupPopup::timerEvent(QTimerEvent * /*event*/)
{
    for (auto * const widget : findChildren<LXQtTaskButton *>())
    {
        if (widget->menu())
            return;
    }
    bool button_has_dnd_hover = false;
    QLayout* l = layout();
    for (int i = l->count(); i--;)
    {
        auto * const button = reinterpret_cast<LXQtTaskButton const *>(l->itemAt(i)->widget());
        if (button && button->hasDragAndDropHover())
        {
            button_has_dnd_hover = true;
            break;
        }
    }
    if (!button_has_dnd_hover)
        close();
}
