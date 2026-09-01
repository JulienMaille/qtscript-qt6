/****************************************************************************
**
** QuickJS-NG implementation of QScriptValueIterator.
**
****************************************************************************/

#include "qscriptquickjs_p.h"

#include <QtScript/qscriptvalueiterator.h>

#include <QtCore/qstringlist.h>

QT_BEGIN_NAMESPACE

class QScriptValueIteratorPrivate
{
public:
    QScriptValueIteratorPrivate() = default;

    QScriptEnginePrivate *enginePrivate() const
    {
        return QScriptEnginePrivate::get(objectValue.engine());
    }

    bool isUsable() const
    {
        const QScriptEnginePrivate *engine = enginePrivate();
        return objectValue.isObject() && engine && engine->state && engine->state->context;
    }

    bool hasCurrent() const
    {
        return isUsable() && initialized && current >= 0 && current < propertyNames.size();
    }

    void ensureInitialized()
    {
        if (initialized)
            return;

        initialized = true;
        if (!isUsable())
            return;

        QScriptEnginePrivate *engine = enginePrivate();
        JSContext *context = engine->state->context;
        bool ok = false;
        JSValue object = engine->toQuickJS(objectValue, &ok);
        if (!ok || !JS_IsObject(object)) {
            if (ok)
                JS_FreeValue(context, object);
            return;
        }

        JSPropertyEnum *properties = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context, &properties, &count, object,
                                   JS_GPN_STRING_MASK) >= 0) {
            propertyNames.reserve(int(count));
            for (uint32_t index = 0; index < count; ++index) {

                JSValue name = JS_AtomToString(context, properties[index].atom);
                if (!JS_IsException(name)) {
                    const QString propertyName = qScriptQuickJSString(context, name);
                    bool skip = propertyName == QLatin1String("__qtscript_static_scope__");
                    if (!propertyName.contains(QLatin1Char('('))) {
                        const QScriptValue property = objectValue.property(
                            propertyName, QScriptValue::ResolveLocal);
                        if (property.isFunction()
                            && property.property(
                                   QStringLiteral("__qtscript_qobject_method__"),
                                   QScriptValue::ResolveLocal).isString())
                            skip = true;
                    }
                    if (!skip)
                        propertyNames.append(propertyName);
                }
                JS_FreeValue(context, name);
            }
            JS_FreePropertyEnum(context, properties, count);
        } else if (JS_HasException(context)) {
            JS_FreeValue(context, JS_GetException(context));
        }

        JS_FreeValue(context, object);
    }

    QScriptValue objectValue;
    QStringList propertyNames;
    int position = 0;
    int current = -1;
    bool initialized = false;
};

QScriptValueIterator::QScriptValueIterator(const QScriptValue &object)
{
    if (object.isObject()) {
        d_ptr.reset(new QScriptValueIteratorPrivate);
        d_ptr->objectValue = object;
    }
}

QScriptValueIterator::~QScriptValueIterator() = default;

bool QScriptValueIterator::hasNext() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->isUsable())
        return false;

    const_cast<QScriptValueIteratorPrivate *>(d)->ensureInitialized();
    return d->position < d->propertyNames.size();
}

void QScriptValueIterator::next()
{
    Q_D(QScriptValueIterator);
    if (!d)
        return;

    d->ensureInitialized();
    if (d->position >= d->propertyNames.size())
        return;

    d->current = d->position;
    ++d->position;
}

bool QScriptValueIterator::hasPrevious() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->isUsable())
        return false;

    const_cast<QScriptValueIteratorPrivate *>(d)->ensureInitialized();
    return d->position > 0;
}

void QScriptValueIterator::previous()
{
    Q_D(QScriptValueIterator);
    if (!d)
        return;

    d->ensureInitialized();
    if (d->position <= 0)
        return;

    --d->position;
    d->current = d->position;
}

void QScriptValueIterator::toFront()
{
    Q_D(QScriptValueIterator);
    if (!d)
        return;

    d->ensureInitialized();
    d->position = 0;
}

void QScriptValueIterator::toBack()
{
    Q_D(QScriptValueIterator);
    if (!d)
        return;

    d->ensureInitialized();
    d->position = d->propertyNames.size();
}

QString QScriptValueIterator::name() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return QString();
    return d->propertyNames.at(d->current);
}

QScriptString QScriptValueIterator::scriptName() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return QScriptString();

    QScriptEngine *engine = d->objectValue.engine();
    return engine ? engine->toStringHandle(d->propertyNames.at(d->current))
                  : QScriptString();
}

QScriptValue QScriptValueIterator::value() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return QScriptValue();
    return d->objectValue.property(d->propertyNames.at(d->current),
                                   QScriptValue::ResolveLocal);
}

void QScriptValueIterator::setValue(const QScriptValue &value)
{
    Q_D(QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return;
    d->objectValue.setProperty(d->propertyNames.at(d->current), value,
                               QScriptValue::KeepExistingFlags);
}

QScriptValue::PropertyFlags QScriptValueIterator::flags() const
{
    Q_D(const QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return {};
    return d->objectValue.propertyFlags(d->propertyNames.at(d->current),
                                        QScriptValue::ResolveLocal);
}

void QScriptValueIterator::remove()
{
    Q_D(QScriptValueIterator);
    if (!d || !d->hasCurrent())
        return;

    const QString propertyName = d->propertyNames.at(d->current);
    QScriptEnginePrivate *engine = d->enginePrivate();
    if (engine && engine->state && engine->state->context) {
        JSContext *context = engine->state->context;
        bool ok = false;
        JSValue object = engine->toQuickJS(d->objectValue, &ok);
        if (ok && JS_IsObject(object)) {
            const QByteArray utf8 = propertyName.toUtf8();
            JSAtom atom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
            if (atom != JS_ATOM_NULL) {
                JS_DeleteProperty(context, object, atom, 0);
                JS_FreeAtom(context, atom);
            }
            JS_FreeValue(context, object);
        } else if (ok) {
            JS_FreeValue(context, object);
        }
        if (JS_HasException(context))
            JS_FreeValue(context, JS_GetException(context));
    }

    // Keep the cursor on the same logical side of the removed item.  This
    // makes removing during forward iteration continue with the next item,
    // while removing during reverse iteration continues with the preceding
    // item.
    if (d->position > d->current)
        --d->position;
    d->propertyNames.removeAt(d->current);
    d->current = -1;
}

QScriptValueIterator &QScriptValueIterator::operator=(QScriptValue &object)
{
    d_ptr.reset();
    if (object.isObject()) {
        d_ptr.reset(new QScriptValueIteratorPrivate);
        d_ptr->objectValue = object;
    }
    return *this;
}

QT_END_NAMESPACE
