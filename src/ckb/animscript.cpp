#include <cmath>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QUrl>
#include "animscript.h"

QHash<QUuid, AnimScript*> AnimScript::scripts;

AnimScript::AnimScript(QObject* parent, const QString& path) :
    QObject(parent), _path(path), initialized(false), process(0)
{
}

AnimScript::AnimScript(QObject* parent, const AnimScript& base) :
    QObject(parent), _info(base._info), _path(base._path), initialized(false), process(0)
{
}

AnimScript::~AnimScript(){
    if(process){
        process->kill();
        process->waitForFinished(1000);
        delete process;
    }
}

QString AnimScript::path(){
#ifdef __APPLE__
    return QDir(QApplication::applicationDirPath() + "/../Resources").absoluteFilePath("ckb-animations");
#else
    return QDir(QApplication::applicationDirPath()).absoluteFilePath("ckb-animations");
#endif
}

void AnimScript::scan(){
    QDir dir(path());
    foreach(AnimScript* script, scripts)
        delete script;
    scripts.clear();
    foreach(QString file, dir.entryList(QDir::Files | QDir::Executable)){
        AnimScript* script = new AnimScript(qApp, dir.absoluteFilePath(file));
        if(script->load() && !scripts.contains(script->_info.guid))
            scripts[script->_info.guid] = script;
        else
            delete script;
    }
}

QList<const AnimScript*> AnimScript::list(){
    // Gather the animations into an alphabetically-sorted list
    QMap<QString, const AnimScript*> result;
    foreach(AnimScript* script, scripts.values()){
        QString name = script->name();
        if(result.contains(name)){
            // If duplicate names exist, make them unique by including their GUIDs
            AnimScript* last = (AnimScript*)result[name];
            last->_info.name += " " + last->guid().toString().toUpper();
            script->_info.name += " " + script->guid().toString().toUpper();
        }
        result[script->name()] = script;
    }
    return result.values();
}

AnimScript* AnimScript::copy(QObject* parent, const QUuid& id){
     return scripts.contains(id) ? new AnimScript(parent, *scripts.value(id)) : 0;
}

inline QString urlParam(const QString& param){
    return QUrl::fromPercentEncoding(param.trimmed().toLatin1()).trimmed();
}

const static double ONE_DAY = 24. * 60. * 60.;

bool AnimScript::load(){
    // Run the process to get script info
    QProcess infoProcess;
    infoProcess.start(_path, QStringList("--ckb-info"));
    qDebug() << "Scanning " << _path;
    infoProcess.waitForFinished(1000);
    if(infoProcess.state() == QProcess::Running){
        // Kill the process if it takes more than 1s
        infoProcess.kill();
        return false;
    }
    // Set defaults for performance info
    _info.kpMode = KP_NONE;
    _info.absoluteTime = _info.preempt = _info.liveParams = false;
    _info.repeat = true;
    double defaultDuration = -1.;
    // Read output
    QString line;
    while((line = infoProcess.readLine()) != ""){
        line = line.trimmed();
        QStringList components = line.split(" ");
        if(components.count() < 2)
            continue;
        QString param = components[0].trimmed();
        if(param == "guid")
            _info.guid = QUuid(urlParam(components[1]));
        else if(param == "name")
            _info.name = urlParam(components[1]);
        else if(param == "version")
            _info.version = urlParam(components[1]);
        else if(param == "year")
            _info.year = urlParam(components[1]);
        else if(param == "author")
            _info.author = urlParam(components[1]);
        else if(param == "license")
            _info.license = urlParam(components[1]);
        else if(param == "description")
            _info.description = urlParam(components[1]);
        else if(param == "kpmode")
            _info.kpMode = (components[1] == "position") ? KP_POSITION : (components[1] == "name") ? KP_NAME : KP_NONE;
        else if(param == "time"){
            if(defaultDuration > 0.)
                // Can't specify absolute time if a duration is included
                continue;
            _info.absoluteTime = (components[1] == "absolute");
        } else if(param == "repeat")
            _info.repeat = (components[1] == "on");
        else if(param == "preempt")
            _info.preempt = (components[1] == "on");
        else if(param == "parammode")
            _info.liveParams = (components[1] == "live");
        else if(param == "param"){
            // Read parameter
            if(components.count() < 3)
                continue;
            while(components.count() < 8)
                components.append("");
            Param::Type type = Param::INVALID;
            QString sType = components[1].toLower();
            if(sType == "long")
                type = Param::LONG;
            else if(sType == "double")
                type = Param::DOUBLE;
            else if(sType == "bool")
                type = Param::BOOL;
            else if(sType == "rgb")
                type = Param::RGB;
            else if(sType == "argb")
                type = Param::ARGB;
            else if(sType == "gradient")
                type = Param::GRADIENT;
            else if(sType == "agradient")
                type = Param::AGRADIENT;
            else if(sType == "angle")
                type = Param::ANGLE;
            else if(sType == "string")
                type = Param::STRING;
            else if(sType == "label")
                type = Param::LABEL;
            else
                continue;
            // "param <type> <name> <prefix> <postfix> <default>"
            QString name = components[2].toLower();
            // Make sure it's not present already
            if(hasParam(name))
                continue;
            QString prefix = urlParam(components[3]), postfix = urlParam(components[4]);
            QVariant def = urlParam(components[5]), minimum = urlParam(components[6]), maximum = urlParam(components[7]);
            // Check types of predefined parameters
            if((name == "trigger" || name == "kptrigger") && type != Param::BOOL)
                continue;
            else if(name == "duration"){
                // For duration, also set min/max appropriately and make sure value is sane
                double value = def.toDouble();
                if(_info.absoluteTime || type != Param::DOUBLE || value < 0.1 || value > ONE_DAY)
                    continue;
                minimum = 0.1;
                maximum = ONE_DAY;
                defaultDuration = value;
            } else if(name == "delay" || name == "kpdelay" || name == "repeat" || name == "kprepeat" || name == "stop" || name == "kpstop" || name == "kprelease")
                // Other predefined params may not be specified here
                continue;
            Param param = { type, name, prefix, postfix, def, minimum, maximum };
            _info.params.append(param);
        }
    }
    // Make sure the required parameters are filled out
    if(_info.guid.isNull() || _info.name == "" || _info.version == "" || _info.year == "" || _info.author == "" || _info.license == "")
        return false;
    // Add timing parameters
    if(!hasParam("trigger")){
        Param trigger = { Param::BOOL, "trigger", "", "", true, 0, 0 };
        _info.params.append(trigger);
    }
    if(!hasParam("kptrigger")){
        Param kptrigger = { Param::BOOL, "kptrigger", "", "", false, 0, 0 };
        _info.params.append(kptrigger);
    }
    if(_info.absoluteTime || !_info.repeat)
        _info.preempt = false;
    Param delay = { Param::DOUBLE, "delay", "", "", 0., 0., ONE_DAY };
    Param kpdelay = { Param::DOUBLE, "kpdelay", "", "", 0., 0., ONE_DAY };
    Param kprelease = { Param::BOOL, "kprelease", "", "", false, 0, 03 };
    _info.params.append(delay);
    _info.params.append(kpdelay);
    _info.params.append(kprelease);
    if(defaultDuration < 0.){
        // If relative time is used but no duration is given, default to 1.0s
        defaultDuration = 1.;
        if(!_info.absoluteTime){
            Param duration = { Param::DOUBLE, "duration", "", "", defaultDuration, 0.1, ONE_DAY };
            _info.params.append(duration);
        }
    }
    if(_info.repeat){
        Param repeat = { Param::DOUBLE, "repeat", "", "", defaultDuration, 0.1, ONE_DAY };
        Param kprepeat = { Param::DOUBLE, "kprepeat", "", "", defaultDuration, 0.1, ONE_DAY };
        // When repeats are enabled, stop and kpstop are LONG values (number of repeats)
        Param stop = { Param::LONG, "stop", "", "", -1, 0, 1000 };
        Param kpstop = { Param::LONG, "kpstop", "", "", 0, 0, 1000 };
        _info.params.append(repeat);
        _info.params.append(kprepeat);
        _info.params.append(stop);
        _info.params.append(kpstop);
    } else {
        // When repeats are disabled, stop and kpstop are DOUBLE values (seconds)
        Param stop = { Param::DOUBLE, "stop", "", "", -1., 0.1, ONE_DAY };
        Param kpstop = { Param::DOUBLE, "kpstop", "", "", -1., 0.1, ONE_DAY };
        _info.params.append(stop);
        _info.params.append(kpstop);
    }
    return true;
}

void AnimScript::init(const KeyMap& map, const QStringList& keys, const QMap<QString, QVariant>& paramValues){
    if(_path == "")
        return;
    stop();
    _map = map;
    _keys = keys;
    _paramValues = paramValues;
    setDuration();
    stopped = firstFrame = false;
    initialized = true;
}

void AnimScript::setDuration(){
    if(_info.absoluteTime){
        durationMsec = 1000;
        repeatMsec = 0;
    } else {
        durationMsec = round(_paramValues.value("duration").toDouble() * 1000.);
        if(durationMsec <= 0)
            durationMsec = -1;
        repeatMsec = round(_paramValues.value("repeat").toDouble() * 1000.);
    }
}

void AnimScript::parameters(const QMap<QString, QVariant>& paramValues){
    if(!initialized || !process || !_info.liveParams)
        return;
    _paramValues = paramValues;
    setDuration();
    printParams();
}

void AnimScript::printParams(){
    process->write("begin params\n");
    QMapIterator<QString, QVariant> i(_paramValues);
    while(i.hasNext()){
        i.next();
        process->write("param ");
        process->write(i.key().toLatin1());
        process->write(" ");
        process->write(QUrl::toPercentEncoding(i.value().toString()));
        process->write("\n");
    }
    process->write("end params\n");
}

void AnimScript::start(quint64 timestamp){
    if(!initialized)
        return;
    stop();
    stopped = firstFrame = readFrame = readAnyFrame = false;
    process = new QProcess(this);
    connect(process, SIGNAL(readyRead()), this, SLOT(readProcess()));
    process->start(_path, QStringList("--ckb-run"));
    qDebug() << "Starting " << _path;
    // Determine the upper left corner of the given keys
    QStringList keysCopy = _keys;
    minX = INT_MAX;
    minY = INT_MAX;
    foreach(const QString& key, keysCopy){
        const KeyPos* pos = _map.key(key);
        if(!pos){
            keysCopy.removeAll(key);
            continue;
        }
        if(pos->x < minX)
            minX = pos->x;
        if(pos->y < minY)
            minY = pos->y;
    }
    // Write the keymap to the process
    process->write("begin keymap\n");
    process->write(QString("keycount %1\n").arg(keysCopy.count()).toLatin1());
    foreach(const QString& key, keysCopy){
        const KeyPos* pos = _map.key(key);
        process->write(QString("key %1 %2,%3\n").arg(key).arg(pos->x - minX).arg(pos->y - minY).toLatin1());
    }
    process->write("end keymap\n");
    // Write parameters
    printParams();
    // Begin animating
    process->write("begin run\n");
    lastFrame = timestamp;
}

void AnimScript::retrigger(quint64 timestamp, bool allowPreempt){
    if(!initialized)
        return;
    if(allowPreempt && _info.preempt && repeatMsec > 0)
        // If preemption is wanted, trigger the animation 1 duration in the past first
        retrigger(timestamp - repeatMsec);
    if(!process)
        start(timestamp);
    nextFrame(timestamp);
    process->write("start\n");
}

void AnimScript::keypress(const QString& key, bool pressed, quint64 timestamp){
    if(!initialized)
        return;
    if(!process)
        start(timestamp);
    switch(_info.kpMode){
    case KP_NONE:
        // If KPs aren't allowed, call retrigger instead
        if(pressed)
            retrigger(timestamp);
        break;
    case KP_NAME:
        // Print keypress by name
        nextFrame(timestamp);
        process->write(("key " + key + (pressed ? " down\n" : " up\n")).toLatin1());
        break;
    case KP_POSITION:
        // Print keypress by position
        const KeyPos* kp = _map.key(key);
        if(!kp)
            return;
        nextFrame(timestamp);
        process->write(("key " + QString("%1,%2").arg(kp->x - minX).arg(kp->y - minY) + (pressed ? " down\n" : " up\n")).toLatin1());
        break;
    }
}

void AnimScript::stop(){
    _colors.clear();
    if(process){
        process->kill();
        connect(process, SIGNAL(finished(int)), process, SLOT(deleteLater()));
        disconnect(process, SIGNAL(readyRead()), this, SLOT(readProcess()));
        process = 0;
    }
}

void AnimScript::readProcess(){
    while(process->canReadLine()){
        QString line = process->readLine().trimmed();
        if(inputBuffer.length() == 0 && line != "begin frame"){
            // Ignore anything not between "begin frame" and "end frame", except for "end run", which indicates that the program is done.
            if(line == "end run"){
                stopped = true;
                return;
            }
            continue;
        }
        if(line == "end frame"){
            // Process this frame
            foreach(QString input, inputBuffer){
                QStringList split = input.split(" ");
                if(split.length() != 3 || split[0] != "argb")
                    continue;
                _colors[split[1]] = split[2].toUInt(0, 16);
            }
            inputBuffer.clear();
            readFrame = readAnyFrame = true;
            continue;
        }
        inputBuffer += line;
    }
}

void AnimScript::frame(quint64 timestamp){
    if(!initialized || stopped)
        return;
    // Start the animation if it's not running yet
    if(!process)
        start(timestamp);

    // If at least one frame was read (or no frame commands have been sent yet), advance the animation
    if(readFrame || !firstFrame)
        nextFrame(timestamp);
    readFrame = false;
}

void AnimScript::nextFrame(quint64 timestamp){
    if(timestamp <= lastFrame)
        lastFrame = timestamp;
    double delta = (timestamp - lastFrame) / (double)durationMsec;
    // Skip any complete durations
    if(!_info.absoluteTime){
        while(delta > 1.){
            process->write("frame 1\n");
            delta--;
        }
    }
    if(delta < 0.)
        delta = 0.;
    lastFrame = timestamp;
    process->write(QString("frame %1\n").arg(delta).toLatin1());
    firstFrame = true;
}
