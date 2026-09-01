/****************************************************************************
**
** Minimal private declarative helpers retained for the QtScript test API.
**
****************************************************************************/

#include "../bridge/qscriptdeclarativeclass_p.h"
#include "../api/qscriptengine.h"

QT_BEGIN_NAMESPACE

QScriptValue QScriptDeclarativeClass::newStaticScopeObject(
    QScriptEngine *engine, int propertyCount, const QString *names,
    const QScriptValue *values, const QScriptValue::PropertyFlags *flags)
{
    if (!engine || propertyCount < 0)
        return QScriptValue();
    QScriptValue object = engine->newObject();
    for (int index = 0; index < propertyCount; ++index) {
        const QScriptValue::PropertyFlags propertyFlags = flags
            ? flags[index] : QScriptValue::PropertyFlags();
        object.setProperty(names[index], values[index],
                           propertyFlags | QScriptValue::Undeletable);
    }
    object.setProperty(QStringLiteral("__qtscript_static_scope__"), true,
                       QScriptValue::ReadOnly | QScriptValue::Undeletable
                           | QScriptValue::SkipInEnumeration);
    return object;
}

QScriptValue QScriptDeclarativeClass::newStaticScopeObject(QScriptEngine *engine)
{
    return newStaticScopeObject(engine, 0, nullptr, nullptr, nullptr);
}

QT_END_NAMESPACE
