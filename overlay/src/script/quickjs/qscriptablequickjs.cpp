/****************************************************************************
**
** QScriptable support for the QuickJS-NG QtScript backend.
**
****************************************************************************/

#include "../api/qscriptable.h"
#include "../api/qscriptable_p.h"
#include "../api/qscriptcontext.h"
#include "../api/qscriptengine.h"

QT_BEGIN_NAMESPACE

QScriptable::QScriptable()
    : d_ptr(new QScriptablePrivate)
{
    d_ptr->q_ptr = this;
}

QScriptable::~QScriptable() = default;

QScriptEngine *QScriptable::engine() const
{
    Q_D(const QScriptable);
    return d->engine;
}

QScriptContext *QScriptable::context() const
{
    return engine() ? engine()->currentContext() : nullptr;
}

QScriptValue QScriptable::thisObject() const
{
    return context() ? context()->thisObject() : QScriptValue();
}

int QScriptable::argumentCount() const
{
    return context() ? context()->argumentCount() : -1;
}

QScriptValue QScriptable::argument(int index) const
{
    return context() ? context()->argument(index) : QScriptValue();
}

QT_END_NAMESPACE
