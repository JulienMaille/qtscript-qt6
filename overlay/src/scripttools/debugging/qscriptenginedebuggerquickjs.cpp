/****************************************************************************
**
** Lightweight QtScriptTools facade for the QuickJS-NG backend.
**
****************************************************************************/

#include "qscriptenginedebugger.h"

#include <QtScript/qscriptengine.h>
#include <QtCore/QPointer>
#include <QtCore/private/qobject_p.h>
#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <array>

QT_BEGIN_NAMESPACE

class QScriptEngineDebuggerPrivate : public QObjectPrivate
{
    Q_DECLARE_PUBLIC(QScriptEngineDebugger)
public:
    ~QScriptEngineDebuggerPrivate() override
    {
        for (const QPointer<QWidget> &widget : widgets) {
            if (widget && !widget->parent())
                delete widget;
        }
        if (standardWindow && !standardWindow->parent())
            delete standardWindow;
    }

    QPointer<QScriptEngine> engine;
    bool autoShow = true;
    std::array<QPointer<QAction>, 16> actions{};
    std::array<QPointer<QWidget>, 9> widgets{};
    QPointer<QMainWindow> standardWindow;
};

namespace {

QString actionText(QScriptEngineDebugger::DebuggerAction action)
{
    static const char *const names[] = {
        "Interrupt", "Continue", "Step Into", "Step Over", "Step Out",
        "Run to Cursor", "Run to New Script", "Toggle Breakpoint",
        "Clear Debug Output", "Clear Error Log", "Clear Console",
        "Find in Script", "Find Next", "Find Previous", "Go to Line"
    };
    const int index = int(action);
    return QScriptEngineDebugger::tr(index >= 0 && index < 15 ? names[index] : "Debugger Action");
}

QString widgetText(QScriptEngineDebugger::DebuggerWidget widget)
{
    static const char *const names[] = {
        "Console", "Stack", "Scripts", "Locals", "Code", "Code Finder",
        "Breakpoints", "Debug Output", "Error Log"
    };
    const int index = int(widget);
    return QScriptEngineDebugger::tr(index >= 0 && index < 9 ? names[index] : "Debugger");
}

} // unnamed namespace

QScriptEngineDebugger::QScriptEngineDebugger(QObject *parent)
    : QObject(*new QScriptEngineDebuggerPrivate, parent)
{
}

QScriptEngineDebugger::~QScriptEngineDebugger() = default;

void QScriptEngineDebugger::attachTo(QScriptEngine *engine)
{
    Q_D(QScriptEngineDebugger);
    d->engine = engine;
}

void QScriptEngineDebugger::detach()
{
    Q_D(QScriptEngineDebugger);
    d->engine.clear();
}

bool QScriptEngineDebugger::autoShowStandardWindow() const
{
    Q_D(const QScriptEngineDebugger);
    return d->autoShow;
}

void QScriptEngineDebugger::setAutoShowStandardWindow(bool autoShow)
{
    Q_D(QScriptEngineDebugger);
    d->autoShow = autoShow;
}

#ifndef QT_NO_MAINWINDOW
QMainWindow *QScriptEngineDebugger::standardWindow() const
{
    Q_D(const QScriptEngineDebugger);
    auto *mutableD = const_cast<QScriptEngineDebuggerPrivate *>(d);
    if (d->standardWindow)
        return d->standardWindow;
    if (!QApplication::instance())
        return nullptr;

    auto *that = const_cast<QScriptEngineDebugger *>(this);
    auto *window = new QMainWindow;
    window->setWindowTitle(tr("Qt Script Debugger (QuickJS compatibility mode)"));
    auto *central = new QWidget(window);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(that->widget(CodeWidget));
    window->setCentralWidget(central);
    window->addToolBar(that->createStandardToolBar(window));
    mutableD->standardWindow = window;
    return window;
}
#endif

QToolBar *QScriptEngineDebugger::createStandardToolBar(QWidget *parent)
{
    auto *toolbar = new QToolBar(parent);
    toolbar->addAction(action(InterruptAction));
    toolbar->addAction(action(ContinueAction));
    toolbar->addSeparator();
    toolbar->addAction(action(StepIntoAction));
    toolbar->addAction(action(StepOverAction));
    toolbar->addAction(action(StepOutAction));
    return toolbar;
}

QMenu *QScriptEngineDebugger::createStandardMenu(QWidget *parent)
{
    auto *menu = new QMenu(tr("Debug"), parent);
    menu->addAction(action(InterruptAction));
    menu->addAction(action(ContinueAction));
    menu->addSeparator();
    menu->addAction(action(StepIntoAction));
    menu->addAction(action(StepOverAction));
    menu->addAction(action(StepOutAction));
    return menu;
}

QWidget *QScriptEngineDebugger::widget(DebuggerWidget widgetType) const
{
    Q_D(const QScriptEngineDebugger);
    auto *mutableD = const_cast<QScriptEngineDebuggerPrivate *>(d);
    const int index = int(widgetType);
    if (index < 0 || index >= int(d->widgets.size()))
        return nullptr;
    if (!d->widgets[size_t(index)]) {
        auto *container = new QWidget;
        container->setObjectName(QStringLiteral("qtscript_quickjs_debugger_widget_%1").arg(index));
        auto *layout = new QVBoxLayout(container);
        layout->addWidget(new QLabel(widgetText(widgetType), container));
        mutableD->widgets[size_t(index)] = container;
    }
    return d->widgets[size_t(index)];
}

QAction *QScriptEngineDebugger::action(DebuggerAction actionType) const
{
    Q_D(const QScriptEngineDebugger);
    auto *mutableD = const_cast<QScriptEngineDebuggerPrivate *>(d);
    const int index = int(actionType);
    if (index < 0 || index >= int(d->actions.size()))
        return nullptr;
    if (!d->actions[size_t(index)]) {
        auto *that = const_cast<QScriptEngineDebugger *>(this);
        auto *created = new QAction(actionText(actionType), that);
        created->setObjectName(QStringLiteral("qtscript_quickjs_debugger_action_%1").arg(index));
        mutableD->actions[size_t(index)] = created;
    }
    return d->actions[size_t(index)];
}

QScriptEngineDebugger::DebuggerState QScriptEngineDebugger::state() const
{
    return RunningState;
}

QT_END_NAMESPACE

#include "moc_qscriptenginedebugger.cpp"
