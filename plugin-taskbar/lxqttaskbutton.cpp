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

#include "lxqttaskbutton.h"
#include "lxqttaskgroup.h"
#include "lxqttaskbar.h"

#include <LXQt/Settings>

#include <QDebug>
#include <XdgIcon>
#include <QTimer>
#include <QThread>
#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>
#include <QPainter>
#include <QDrag>
#include <QMouseEvent>
#include <QMimeData>
#include <QApplication>
#include <QDragEnterEvent>
#include <QStylePainter>
#include <QStyleOptionToolButton>
#include <QDesktopWidget>
#include <QScreen>

#include "lxqttaskbutton.h"
#include "lxqttaskgroup.h"
#include "lxqttaskbar.h"

#include <KWindowSystem/KWindowSystem>
// Necessary for closeProcess()
#include <signal.h>

bool LXQtTaskButton::sDraggging = false;

/************************************************

************************************************/
void LeftAlignedTextStyle::drawItemText(QPainter * painter, const QRect & rect, int flags
            , const QPalette & pal, bool enabled, const QString & text
            , QPalette::ColorRole textRole) const
{
    QString txt = text;
    // get the button text because the text that's given to this function may be middle-elided
    if (const QToolButton *tb = dynamic_cast<const QToolButton*>(painter->device()))
        txt = tb->text();
    txt = QFontMetrics(painter->font()).elidedText(txt, Qt::ElideRight, rect.width());
    QProxyStyle::drawItemText(painter, rect, (flags & ~Qt::AlignHCenter) | Qt::AlignLeft, pal, enabled, txt, textRole);
}


/************************************************

************************************************/
LXQtTaskButton::LXQtTaskButton(const WId window, LXQtTaskBar * taskbar, QWidget *parent) :
    QToolButton(parent),
    mWindow(window),
    mWMMoveResize(NET::MoveResizeCancel),
    mUrgencyHint(false),
    mOrigin(Qt::TopLeftCorner),
    mParentTaskBar(taskbar),
    mPlugin(mParentTaskBar->plugin()),
    mIconSize(mPlugin->panel()->iconSize()),
    mWheelDelta(0),
    mDNDTimer(new QTimer(this)),
    mWheelTimer(new QTimer(this))
{
    Q_ASSERT(taskbar);

    setCheckable(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    setMinimumWidth(1);
    setMinimumHeight(1);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setAcceptDrops(true);

    updateText();
    updateIcon();

    mDNDTimer->setSingleShot(true);
    mDNDTimer->setInterval(700);
    connect(mDNDTimer, &QTimer::timeout, this, &LXQtTaskButton::activateWithDraggable);

    mWheelTimer->setSingleShot(true);
    mWheelTimer->setInterval(250);
    connect(mWheelTimer, &QTimer::timeout, this, [this] {
        mWheelDelta = 0; // forget previous wheel deltas
    });

    connect(LXQt::Settings::globalSettings(), &LXQt::GlobalSettings::iconThemeChanged, this, &LXQtTaskButton::updateIcon);
    connect(mParentTaskBar,                   &LXQtTaskBar::iconByClassChanged,        this, &LXQtTaskButton::updateIcon);
}

/************************************************

************************************************/
LXQtTaskButton::~LXQtTaskButton() = default;

/************************************************

 ************************************************/
void LXQtTaskButton::updateText()
{
    KWindowInfo info(mWindow, NET::WMVisibleName | NET::WMName);
    QString title = info.visibleName().isEmpty() ? info.name() : info.visibleName();
    setText(title.replace(QStringLiteral("&"), QStringLiteral("&&")));
    setToolTip(title);
}

/************************************************

 ************************************************/
void LXQtTaskButton::updateIcon()
{
    QIcon ico;
    if (mParentTaskBar->isIconByClass())
    {
        ico = XdgIcon::fromTheme(QString::fromUtf8(KWindowInfo{mWindow, NET::Properties(), NET::WM2WindowClass}.windowClassClass()).toLower());
    }
    if (ico.isNull())
    {
        int devicePixels = mIconSize * devicePixelRatioF();
        ico = KWindowSystem::icon(mWindow, devicePixels, devicePixels);
    }
    setIcon(ico.isNull() ? XdgIcon::defaultApplicationIcon() : ico);
}

/************************************************

 ************************************************/
void LXQtTaskButton::refreshIconGeometry(QRect const & geom)
{
    QPixmap &&icon = KWindowSystem::icon(windowId());
    if (icon.rect() != geom)
        KWindowSystem::setIcons(windowId(), icon.scaled(geom.size()), icon);
}

/************************************************

 ************************************************/
void LXQtTaskButton::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::StyleChange)
    {
        // When the icon size changes, the panel doesn't emit any specific
        // signal, but it triggers a stylesheet update, which we can detect
        int newIconSize = mPlugin->panel()->iconSize();
        if (newIconSize != mIconSize)
        {
            mIconSize = newIconSize;
            updateIcon();
        }
    }

    QToolButton::changeEvent(event);
}

/************************************************

 ************************************************/
void LXQtTaskButton::dragEnterEvent(QDragEnterEvent *event)
{
    // It must be here otherwise dragLeaveEvent and dragMoveEvent won't be called
    // on the other hand drop and dragmove events of parent widget won't be called
    event->acceptProposedAction();
    if (event->mimeData()->hasFormat(mimeDataFormat()))
    {
        emit dragging(event->source(), event->pos());
        setAttribute(Qt::WA_UnderMouse, false);
    } else
    {
        mDNDTimer->start();
    }

    QToolButton::dragEnterEvent(event);
}

void LXQtTaskButton::dragMoveEvent(QDragMoveEvent * event)
{
    if (event->mimeData()->hasFormat(mimeDataFormat()))
    {
        emit dragging(event->source(), event->pos());
        setAttribute(Qt::WA_UnderMouse, false);
    }
}

void LXQtTaskButton::dragLeaveEvent(QDragLeaveEvent *event)
{
    mDNDTimer->stop();
    QToolButton::dragLeaveEvent(event);
}

void LXQtTaskButton::dropEvent(QDropEvent *event)
{
    mDNDTimer->stop();
    if (event->mimeData()->hasFormat(mimeDataFormat()))
    {
        emit dropped(event->source(), event->pos());
        setAttribute(Qt::WA_UnderMouse, false);
    }
    QToolButton::dropEvent(event);
}

/************************************************

 ************************************************/
void LXQtTaskButton::mousePressEvent(QMouseEvent* event)
{
    const Qt::MouseButton b = event->button();

    if (Qt::LeftButton == b)
        mDragStartPosition = event->pos();
    else if (Qt::MidButton == b && parentTaskBar()->closeOnMiddleClick())
        closeApplication();

    QToolButton::mousePressEvent(event);
}

/************************************************

 ************************************************/
void LXQtTaskButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (!sDraggging && event->button() == Qt::LeftButton)
    {
        if (isChecked())
            minimizeApplication();
        else
            raiseApplication();
    }
    QToolButton::mouseReleaseEvent(event);
}

/************************************************

 ************************************************/
void LXQtTaskButton::wheelEvent(QWheelEvent* event)
{
    // ignore wheel event if it is not "raise", "minimize" or "move" window
    if (mParentTaskBar->wheelEventsAction() < 2 || mParentTaskBar->wheelEventsAction() > 5)
        return QToolButton::wheelEvent(event);

    QPoint angleDelta = event->angleDelta();
    Qt::Orientation orient = (qAbs(angleDelta.x()) > qAbs(angleDelta.y()) ? Qt::Horizontal : Qt::Vertical);
    int delta = (orient == Qt::Horizontal ? angleDelta.x() : angleDelta.y());

    if (!mWheelTimer->isActive())
        mWheelDelta += abs(delta);
    else
    {
        // NOTE: We should consider a short delay after the last wheel event
        // in order to distinguish between separate wheel rotations; otherwise,
        // a wheel delta threshold will not make much sense because the delta
        // might have been increased due to a previous and separate wheel rotation.
        mWheelTimer->start();
    }

    if (mWheelDelta < mParentTaskBar->wheelDeltaThreshold())
        return QToolButton::wheelEvent(event);
    else
    {
        mWheelDelta = 0;
        mWheelTimer->start(); // start to distinguish between separate wheel rotations
    }

    int D = delta < 0 ? 1 : -1;

    if (mParentTaskBar->wheelEventsAction() == 4)
    {
        moveApplicationToPrevNextDesktop(D < 0);
    }
    else if (mParentTaskBar->wheelEventsAction() == 5)
    {
        moveApplicationToPrevNextDesktop(D > 0);
    }
    else
    {
        if (mParentTaskBar->wheelEventsAction() == 3)
            D *= -1;
        if (D < 0)
            raiseApplication();
        else if (D > 0)
            minimizeApplication();
    }

    QToolButton::wheelEvent(event);
}

/************************************************

 ************************************************/
QMimeData * LXQtTaskButton::mimeData()
{
    QMimeData *mimedata = new QMimeData;
    QByteArray ba;
    QDataStream stream(&ba,QIODevice::WriteOnly);
    stream << (qlonglong)(mWindow);
    mimedata->setData(mimeDataFormat(), ba);
    return mimedata;
}

/************************************************

 ************************************************/
void LXQtTaskButton::mouseMoveEvent(QMouseEvent* event)
{
    QAbstractButton::mouseMoveEvent(event);
    if (!(event->buttons() & Qt::LeftButton))
        return;

    if ((event->pos() - mDragStartPosition).manhattanLength() < QApplication::startDragDistance())
        return;

    QDrag *drag = new QDrag(this);
    drag->setMimeData(mimeData());
    QIcon ico = icon();
    QPixmap img = ico.pixmap(ico.actualSize({32, 32}));
    drag->setPixmap(img);
    switch (parentTaskBar()->panel()->position())
    {
        case ILXQtPanel::PositionLeft:
        case ILXQtPanel::PositionTop:
            drag->setHotSpot({0, 0});
            break;
        case ILXQtPanel::PositionRight:
        case ILXQtPanel::PositionBottom:
            drag->setHotSpot(img.rect().bottomRight());
            break;
    }

    sDraggging = true;
    drag->exec();

    // if button is dropped out of panel (e.g. on desktop)
    // it is not deleted automatically by Qt
    drag->deleteLater();

    // release mouse appropriately, by positioning the event outside
    // the button rectangle (otherwise, the button will be toggled)
    QMouseEvent releasingEvent(QEvent::MouseButtonRelease, QPoint(-1,-1), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    mouseReleaseEvent(&releasingEvent);

    sDraggging = false;
}

/************************************************

 ************************************************/
bool LXQtTaskButton::isApplicationHidden() const
{
    return KWindowInfo(mWindow, NET::WMState).state() & NET::Hidden;
}

/************************************************

 ************************************************/
bool LXQtTaskButton::isApplicationActive() const
{
    return KWindowInfo(mWindow, {}).win() == KWindowSystem::activeWindow();
}

/************************************************

 ************************************************/
void LXQtTaskButton::activateWithDraggable()
{
    // raise app in any time when there is a drag
    // in progress to allow drop it into an app
    raiseApplication();
    KWindowSystem::forceActiveWindow(mWindow);
}

/************************************************

 ************************************************/
void LXQtTaskButton::raiseApplication()
{
    KWindowInfo info(mWindow, NET::WMDesktop | NET::WMState | NET::XAWMState);
    if (parentTaskBar()->raiseOnCurrentDesktop() && info.isMinimized())
    {
        KWindowSystem::setOnDesktop(mWindow, KWindowSystem::currentDesktop());
    }
    else
    {
        int winDesktop = info.desktop();
        if (winDesktop >= 0 && KWindowSystem::currentDesktop() != winDesktop)
            KWindowSystem::setCurrentDesktop(winDesktop);
    }
    KWindowSystem::activateWindow(mWindow);

    setUrgencyHint(false);
}

/************************************************

 ************************************************/
void LXQtTaskButton::minimizeApplication()
{
    KWindowSystem::minimizeWindow(mWindow);
}

/************************************************

 ************************************************/
void LXQtTaskButton::maximizeApplication()
{
    QAction* act = qobject_cast<QAction*>(sender());
    if (!act)
        return;

    int state = act->data().toInt();
    switch (state)
    {
        case NET::MaxHoriz:
            KWindowSystem::setState(mWindow, NET::MaxHoriz);
            break;

        case NET::MaxVert:
            KWindowSystem::setState(mWindow, NET::MaxVert);
            break;

        default:
            KWindowSystem::setState(mWindow, NET::Max);
            break;
    }

    if (!isApplicationActive())
        raiseApplication();
}

/************************************************

 ************************************************/
void LXQtTaskButton::deMaximizeApplication()
{
    KWindowSystem::clearState(mWindow, NET::Max);

    if (!isApplicationActive())
        raiseApplication();
}

/************************************************

 ************************************************/
void LXQtTaskButton::shadeApplication()
{
    KWindowSystem::setState(mWindow, NET::Shaded);
}

/************************************************

 ************************************************/
void LXQtTaskButton::unShadeApplication()
{
    KWindowSystem::clearState(mWindow, NET::Shaded);
}

/************************************************

 ************************************************/
void LXQtTaskButton::closeApplication()
{
    KWindowInfo(mWindow, NET::CloseWindow);
}

void LXQtTaskButton::closeProcess()
{
    if (pid_t pid = KWindowInfo(mWindow, NET::WMPid).pid())
        kill(pid, SIGTERM);
    else
        closeApplication();
}

/************************************************

 ************************************************/
void LXQtTaskButton::setApplicationLayer()
{
    QAction* act = qobject_cast<QAction*>(sender());
    if (!act)
        return;

    int layer = act->data().toInt();
    switch(layer)
    {
        case NET::KeepAbove:
            KWindowSystem::clearState(mWindow, NET::KeepBelow);
            KWindowSystem::setState(mWindow, NET::KeepAbove);
            break;

        case NET::KeepBelow:
            KWindowSystem::clearState(mWindow, NET::KeepAbove);
            KWindowSystem::setState(mWindow, NET::KeepBelow);
            break;

        default:
            KWindowSystem::clearState(mWindow, NET::KeepBelow);
            KWindowSystem::clearState(mWindow, NET::KeepAbove);
            break;
    }
}

/************************************************

 ************************************************/
void LXQtTaskButton::moveApplicationToDesktop()
{
    QAction* act = qobject_cast<QAction*>(sender());
    if (!act)
        return;

    bool ok;
    int desk = act->data().toInt(&ok);

    if (!ok)
        return;

    KWindowSystem::setOnDesktop(mWindow, desk);
}

/************************************************

 ************************************************/
void LXQtTaskButton::moveApplicationToPrevNextDesktop(bool next)
{
    int deskNum = KWindowSystem::numberOfDesktops();
    if (deskNum <= 1)
        return;
    int targetDesk = KWindowInfo(mWindow, NET::WMDesktop).desktop();
    targetDesk = (targetDesk < 0 ? KWindowSystem::currentDesktop()
                                 : targetDesk) + (next ? 1 : -1);
    // wrap around
    if (targetDesk > deskNum)
        targetDesk = 1;
    else if (targetDesk < 1)
        targetDesk = deskNum;

    KWindowSystem::setOnDesktop(mWindow, targetDesk);
}

/************************************************

 ************************************************/
void LXQtTaskButton::moveApplicationToPrevNextMonitor(bool next)
{
    KWindowInfo info(mWindow, NET::WMDesktop);
    if (!info.isOnCurrentDesktop())
        KWindowSystem::setCurrentDesktop(info.desktop());
    if (isMinimized())
        KWindowSystem::unminimizeWindow(mWindow);
    KWindowSystem::forceActiveWindow(mWindow);
    const QRect& windowGeometry = KWindowInfo(mWindow, NET::WMFrameExtents).frameGeometry();
    QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.size() > 1){
        for (int i = 0; i < screens.size(); ++i)
        {
            QRect screenGeometry = screens[i]->geometry();
            if (screenGeometry.intersects(windowGeometry))
            {
                int targetScreen = i + (next ? 1 : -1);
                if (targetScreen < 0)
                    targetScreen += screens.size();
                else if (targetScreen >= screens.size())
                    targetScreen -= screens.size();
                QRect targetScreenGeometry = screens[targetScreen]->geometry();
                int X = windowGeometry.x() - screenGeometry.x() + targetScreenGeometry.x();
                int Y = windowGeometry.y() - screenGeometry.y() + targetScreenGeometry.y();
                KWindowInfo info(mWindow, NET::WMState | NET::WMGeometry, NET::WM2MoveResizeWindow);
                const NET::States &state = info.state();
                KWindowSystem::clearState(mWindow, NET::MaxHoriz | NET::MaxVert | NET::Max | NET::FullScreen);
                info.setPosition(X, Y);
                QTimer::singleShot(200, this, [this, state]
                {
                    KWindowSystem::setState(mWindow, state);
                    raiseApplication();
                });
                break;
            }
        }
    }
}

/************************************************

 ************************************************/
void LXQtTaskButton::finishMoveResize(QObject *obj)
{
    if (mWMMoveResize != NET::MoveResizeCancel)
    {
        reinterpret_cast<QWidget *>(obj)->close();
        QThread::msleep(25);
        KWindowInfo(mWindow, NET::WMMoveResize | NET::WMDesktop)
            .requestMoveResize(mWMMoveResize);

        mWMMoveResize = NET::MoveResizeCancel;
    }
}

/************************************************

 ************************************************/
void LXQtTaskButton::moveApplication()
{
    KWindowSystem::forceActiveWindow(mWindow);
    mWMMoveResize = NET::Move;
}

/************************************************

 ************************************************/
void LXQtTaskButton::resizeApplication()
{
    KWindowSystem::setState(mWindow, NET::Hidden | NET::Focused);
    KWindowSystem::clearState(mWindow, NET::Hidden);
    KWindowSystem::forceActiveWindow(mWindow);
    mWMMoveResize = static_cast<NET::Direction>(-1);
}

/************************************************

 ************************************************/
void LXQtTaskButton::contextMenuEvent(QContextMenuEvent* event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier))
    {
        event->ignore();
        return;
    }

    KWindowInfo info(mWindow,
                     NET::Properties() | NET::WMState | NET::XAWMState,
                     NET::WM2AllowedActions);
    NET::States state = info.state();

    if (!info.isMinimized())
        state &= ~NET::Hidden;

    QMenu* menu = new QMenu(tr("Application"), mParentTaskBar);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAttribute(Qt::WA_AlwaysStackOnTop);
    menu->setWindowFlag(Qt::Popup);
    QAction* a;

    /* KDE menu *******

      + To &Desktop >
      +     &All Desktops
      +     ---
      +     &1 Desktop 1
      +     &2 Desktop 2
      + &To Current Desktop
        &Move
        Re&size
      + Mi&nimize
      + Ma&ximize
      + &Shade
        Ad&vanced >
            Keep &Above Others
            Keep &Below Others
            Fill screen
        &Layer >
            Always on &top
            &Normal
            Always on &bottom
      ---
      + &Close
    */

    /********** Desktop menu **********/
    int deskNum = KWindowSystem::numberOfDesktops();
    if (deskNum > 1)
    {
        int winDesk = KWindowInfo(mWindow, NET::WMDesktop).desktop();
        QMenu* deskMenu = menu->addMenu(tr("To &Desktop"));

        a = deskMenu->addAction(tr("&All Desktops"));
        a->setData(NET::OnAllDesktops);
        a->setEnabled(winDesk != NET::OnAllDesktops);
        connect(a, &QAction::triggered, this, &LXQtTaskButton::moveApplicationToDesktop);
        deskMenu->addSeparator();

        for (int i = 0; i < deskNum; ++i)
        {
            const auto &deskName = KWindowSystem::desktopName(i).trimmed();

            a = deskMenu->addAction(
                deskName.isEmpty() ? tr("Desktop &%1").arg(i + 1)
                                   : QSL("&%1: %2").arg(i + 1).arg(deskName)
            );
            a->setData(i);
            a->setEnabled(i != winDesk);
            connect(a, &QAction::triggered, this, &LXQtTaskButton::moveApplicationToDesktop);
        }

        int curDesk = KWindowSystem::currentDesktop();
        a = menu->addAction(tr("&To Current Desktop"));
        a->setData(curDesk);
        a->setEnabled(curDesk != winDesk);
        connect(a, &QAction::triggered, this, &LXQtTaskButton::moveApplicationToDesktop);
    }
    /********** Move/Resize **********/
    if (QGuiApplication::screens().size() > 1)
    {
        menu->addSeparator();
        a = menu->addAction(tr("Move To N&ext Monitor"));
        connect(a, &QAction::triggered, this, [this] { moveApplicationToPrevNextMonitor(true); });
        a->setEnabled(info.isOnCurrentDesktop() &&
                      info.actionSupported(NET::ActionMove) &&
                      (!(state & NET::FullScreen) || info.actionSupported(NET::ActionFullScreen)));
        a = menu->addAction(tr("Move To &Previous Monitor"));
        connect(a, &QAction::triggered, this, [this] { moveApplicationToPrevNextMonitor(false); });
    }
    menu->addSeparator();
    connect(menu, &QObject::destroyed, this, &LXQtTaskButton::finishMoveResize);
    a = menu->addAction(tr("&Move"));
    a->setEnabled(//info.isOnCurrentDesktop() &&
                  info.actionSupported(NET::ActionMove) &&
                  !(state & (NET::Max | NET::FullScreen)));
    connect(a, &QAction::triggered, this, &LXQtTaskButton::moveApplication);
    a = menu->addAction(tr("Resi&ze"));
    a->setEnabled(//info.isOnCurrentDesktop() &&
                  info.actionSupported(NET::ActionResize) &&
                  !(state & (NET::Max | NET::FullScreen)));
    connect(a, &QAction::triggered, this, &LXQtTaskButton::resizeApplication);

    /********** State menu **********/
    menu->addSeparator();

    a = menu->addAction(tr("Ma&ximize"));
    a->setEnabled(info.actionSupported(NET::ActionMax) && (!(state & NET::Max) || (state & NET::Hidden)));
    a->setData(NET::Max);
    connect(a, &QAction::triggered, this, &LXQtTaskButton::maximizeApplication);

    if (event->modifiers() & Qt::ShiftModifier)
    {
        a = menu->addAction(tr("Maximize vertically"));
        a->setEnabled(info.actionSupported(NET::ActionMaxVert) && !((state & NET::MaxVert) || (state & NET::Hidden)));
        a->setData(NET::MaxVert);
        connect(a, &QAction::triggered, this, &LXQtTaskButton::maximizeApplication);

        a = menu->addAction(tr("Maximize horizontally"));
        a->setEnabled(info.actionSupported(NET::ActionMaxHoriz) && !((state & NET::MaxHoriz) || (state & NET::Hidden)));
        a->setData(NET::MaxHoriz);
        connect(a, &QAction::triggered, this, &LXQtTaskButton::maximizeApplication);
    }

    a = menu->addAction(tr("&Restore"));
    a->setEnabled(state & (NET::Hidden | NET::MaxHoriz | NET::MaxVert));
    connect(a, &QAction::triggered, this, &LXQtTaskButton::deMaximizeApplication);

    a = menu->addAction(tr("Mi&nimize"));
    a->setEnabled(info.actionSupported(NET::ActionMinimize) && !(state & NET::Hidden));
    connect(a, &QAction::triggered, this, &LXQtTaskButton::minimizeApplication);

    if (state & NET::Shaded)
    {
        a = menu->addAction(tr("Roll down"));
        a->setEnabled(info.actionSupported(NET::ActionShade) && !(state & NET::Hidden));
        connect(a, &QAction::triggered, this, &LXQtTaskButton::unShadeApplication);
    }
    else
    {
        a = menu->addAction(tr("Roll up"));
        a->setEnabled(info.actionSupported(NET::ActionShade) && !(state & NET::Hidden));
        connect(a, &QAction::triggered, this, &LXQtTaskButton::shadeApplication);
    }

    /********** Layer menu **********/
    menu->addSeparator();

    QMenu* layerMenu = menu->addMenu(tr("&Layer"));

    a = layerMenu->addAction(tr("Always on &top"));
    // FIXME: There is no info.actionSupported(NET::ActionKeepAbove)
    a->setEnabled(!(state & NET::KeepAbove));
    a->setData(NET::KeepAbove);
    connect(a, &QAction::triggered, this, &LXQtTaskButton::setApplicationLayer);

    a = layerMenu->addAction(tr("&Normal"));
    a->setEnabled((state & NET::KeepAbove) || (state & NET::KeepBelow));
    // FIXME: There is no NET::KeepNormal, so passing 0
    a->setData(0);
    connect(a, &QAction::triggered, this, &LXQtTaskButton::setApplicationLayer);

    a = layerMenu->addAction(tr("Always on &bottom"));
    // FIXME: There is no info.actionSupported(NET::ActionKeepBelow)
    a->setEnabled(!(state & NET::KeepBelow));
    a->setData(NET::KeepBelow);
    connect(a, &QAction::triggered, this, &LXQtTaskButton::setApplicationLayer);

    /********** Kill menu **********/
    menu->addSeparator();
    a = menu->addAction(XdgIcon::fromTheme(QStringLiteral("process-stop")), tr("&Close"));
    connect(a, &QAction::triggered, this, &LXQtTaskButton::closeApplication);
    if (event->modifiers() & Qt::ShiftModifier)
        connect(a = menu->addAction(XdgIcon::fromTheme(QStringLiteral("process-stop")),
                                    tr("&Close all")),
                &QAction::triggered,
                this,
                &LXQtTaskButton::closeProcess);

    {
        QPoint &&point = parentWidget()->pos();

        menu->setGeometry(
            mParentTaskBar->panel()->calculatePopupWindowPos(
                point.x() || point.y() ? point -= mParentTaskBar->pos()
                                       : std::move(mParentTaskBar->mapToGlobal(pos())),
                menu->sizeHint()
            )
        );
    }
    setMenu(menu);
    mPlugin->willShowWindow(menu);
    menu->show();
    QThread::create([menu]{
        QThread::msleep(25);
        KWindowSystem::setState(menu->winId(), NET::KeepAbove);
    })->start();
}

/************************************************

 ************************************************/
void LXQtTaskButton::setUrgencyHint(bool set)
{
    if (mUrgencyHint == set)
        return;

    if (!set)
        KWindowSystem::demandAttention(mWindow, false);

    mUrgencyHint = set;
    setProperty("urgent", set);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

/************************************************

 ************************************************/
bool LXQtTaskButton::isOnDesktop(int desktop) const
{
    return KWindowInfo(mWindow, NET::WMDesktop).isOnDesktop(desktop);
}

bool LXQtTaskButton::isOnCurrentScreen() const
{
    return QApplication::desktop()->screenGeometry(parentTaskBar()).intersects(KWindowInfo(mWindow, NET::WMFrameExtents).frameGeometry());
}

bool LXQtTaskButton::isMinimized() const
{
    return KWindowInfo(mWindow,NET::WMState | NET::XAWMState).isMinimized();
}

Qt::Corner LXQtTaskButton::origin() const
{
    return mOrigin;
}

void LXQtTaskButton::setOrigin(Qt::Corner newOrigin)
{
    if (mOrigin != newOrigin)
    {
        mOrigin = newOrigin;
        update();
    }
}

void LXQtTaskButton::setAutoRotation(bool value, ILXQtPanel::Position position)
{
    if (value)
    {
        switch (position)
        {
        case ILXQtPanel::PositionTop:
        case ILXQtPanel::PositionBottom:
            setOrigin(Qt::TopLeftCorner);
            break;

        case ILXQtPanel::PositionLeft:
            setOrigin(Qt::BottomLeftCorner);
            break;

        case ILXQtPanel::PositionRight:
            setOrigin(Qt::TopRightCorner);
            break;
        }
    }
    else
        setOrigin(Qt::TopLeftCorner);
}

void LXQtTaskButton::paintEvent(QPaintEvent *event)
{
    if (mOrigin == Qt::TopLeftCorner)
    {
        QToolButton::paintEvent(event);
        return;
    }

    QSize sz = size();
    bool transpose = false;
    QTransform transform;

    switch (mOrigin)
    {
    case Qt::TopLeftCorner:
        break;

    case Qt::TopRightCorner:
        transform.rotate(90.0);
        transform.translate(0.0, -sz.width());
        transpose = true;
        break;

    case Qt::BottomRightCorner:
        transform.rotate(180.0);
        transform.translate(-sz.width(), -sz.height());
        break;

    case Qt::BottomLeftCorner:
        transform.rotate(270.0);
        transform.translate(-sz.height(), 0.0);
        transpose = true;
        break;
    }

    QStylePainter painter(this);
    painter.setTransform(transform);
    QStyleOptionToolButton opt;
    initStyleOption(&opt);
    if (transpose)
        opt.rect = opt.rect.transposed();
    painter.drawComplexControl(QStyle::CC_ToolButton, opt);
}

bool LXQtTaskButton::hasDragAndDropHover() const
{
    return mDNDTimer->isActive();
}
