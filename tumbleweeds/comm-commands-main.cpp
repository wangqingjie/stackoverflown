// https://github.com/KubaO/stackoverflown/tree/master/questions/comm-commands-32486198
#include <QtWidgets>
#include <private/qringbuffer_p.h>

// See http://stackoverflow.com/a/32317276/1329652
/// A simple point-to-point intra-process pipe. The other endpoint can live in any
/// thread.
class AppPipe : public QIODevice {
   Q_OBJECT
   QRingBuffer m_buf;
   void _a_write(const QByteArray & data) {
      if (! openMode() & QIODevice::ReadOnly) return; // We must be readable.
      m_buf.append(data);
      emit hasIncoming(data);
      emit readyRead();
   }
public:
   AppPipe(AppPipe * other, QIODevice::OpenMode mode, QObject * parent = 0) :
      QIODevice(parent) {
      addOther(other);
      open(mode);
   }
   AppPipe(AppPipe * other, QObject * parent = 0) : QIODevice(parent) {
      addOther(other);
   }
   void addOther(AppPipe * other) {
      if (other) connect(this, &AppPipe::hasOutgoing, other, &AppPipe::_a_write);
   }
   void removeOther(AppPipe * other) {
      disconnect(this, &AppPipe::hasOutgoing, other, &AppPipe::_a_write);
   }
   void close() Q_DECL_OVERRIDE {
      QIODevice::close();
      m_buf.clear();
   }
   qint64 writeData(const char * data, qint64 maxSize) Q_DECL_OVERRIDE {
      if (!maxSize) return maxSize;
      hasOutgoing(QByteArray(data, maxSize));
      return maxSize;
   }
   qint64 readData(char * data, qint64 maxLength) Q_DECL_OVERRIDE {
      return m_buf.read(data, maxLength);
   }
   qint64 bytesAvailable() const Q_DECL_OVERRIDE {
      return m_buf.size() + QIODevice::bytesAvailable();
   }
   bool canReadLine() const Q_DECL_OVERRIDE {
      return QIODevice::canReadLine() || m_buf.canReadLine();
   }
   bool isSequential() const Q_DECL_OVERRIDE { return true; }
   Q_SIGNAL void hasOutgoing(const QByteArray &);
   Q_SIGNAL void hasIncoming(const QByteArray &);
};

//

template <typename F>
class GuardedSignalTransition : public QSignalTransition {
   F m_fun;
protected:
   bool eventTest(QEvent * ev) Q_DECL_OVERRIDE {
      return QSignalTransition::eventTest(ev) && m_fun();
   }
public:
   GuardedSignalTransition(const QObject * sender, const char * signal, F && f) :
      QSignalTransition(sender, signal), m_fun(std::move(f)) {}
};

template <typename F> static GuardedSignalTransition<F> *
addTransition(QState * src, QAbstractState *target, const QObject * sender, const char * signal, F && f) {
   auto t = new GuardedSignalTransition<F>(sender, signal, std::move(f));
   t->setTargetState(target);
   src->addTransition(t);
   return t;
}

static bool hasLine(QIODevice * dev, const QByteArray & needle) {
   auto result = false;
   while (dev->canReadLine()) {
      auto line = dev->readLine();
      if (line.contains(needle)) result = true;
   }
   return result;
}

void send(QAbstractState * src, QIODevice * dev, const QByteArray & data) {
   QObject::connect(src, &QState::entered, dev, [dev, data]{
      dev->write(data);
   });
}

QTimer * delay(QState * src, int ms, QAbstractState * dst) {
   auto timer = new QTimer(src);
   timer->setSingleShot(true);
   timer->setInterval(ms);
   QObject::connect(src, &QState::entered, timer, static_cast<void (QTimer::*)()>(&QTimer::start));
   QObject::connect(src, &QState::exited,  timer, &QTimer::stop);
   src->addTransition(timer, SIGNAL(timeout()), dst);
   return timer;
}

void expect(QState * src, QIODevice * dev, const QByteArray & data, QAbstractState * dst,
            int timeout = 0, QAbstractState * dstTimeout = nullptr)
{
   addTransition(src, dst, dev, SIGNAL(readyRead()), [dev, data]{
      return hasLine(dev, data);
   });
   if (timeout) delay(src, timeout, dstTimeout);
}

//

class Stateful;

struct OpBase {
   friend class Stateful;
   virtual void make(Stateful & s) = 0;
   virtual ~OpBase() {}
protected:
   enum Flag {
      MustBeNew = 1,    ///< Requires its own state
      NeedsNext = 2,    ///< Requires a successor state
      NeedsFailure = 4, ///< Requires a failure state
      IsNew = 8,        ///< Has its own state
      IsFailure = 16    ///< Is a failure state
   };
   Q_DECLARE_FLAGS(Flags, Flag)
   Flags m_flags;
   OpBase(Flags f) : m_flags(f) {}
};

struct Param {
   const int m_id;
   explicit Param(int id) : m_id(id) {}
   virtual ~Param() {}
};

class Stateful {
   friend struct OpBase;
   QMap<int, QSharedPointer<Param>> m_params;
   struct Level {
      QAbstractState *cur { nullptr }, *next { nullptr }, *failure { nullptr };
      Level(QAbstractState * c) : cur(c) {}
      Level() {}
   };
   QStack<Level> m_levels;
   QStack<QSharedPointer<OpBase>> m_ops;
   Level & top() { return m_levels.top(); }
   const Level & top() const { return m_levels.top(); }
public:
   Stateful(QAbstractState * parent) {
      m_levels.push(Level(parent));
      m_levels.push(Level());
   }
   ~Stateful() { flush(); }
   template <typename P>
   Stateful & operator*(const P & param) {
      m_params[param.m_id] = QSharedPointer<P>::create(param);
      return *this;
   }
   Param * param(int id) const {
      auto it = m_params.find(id);
      return (it != m_params.end()) ? (*it).data() : nullptr;
   }
   template <typename T> T * param() const {
      return dynamic_cast<T*>(param(T::id()));
   }
   template <typename O>
   Stateful & operator+(const O & op) {
      auto copy = QSharedPointer<O>::create(op);
      copy->m_flags |= OpBase::IsNew;
      m_ops.push(copy);
      return *this;
   }
   template <typename O>
   Stateful & operator|(const O & op) {
      m_ops.push(QSharedPointer<O>::create(op));
      return *this;
   }
   QAbstractState * aState() {
      return top().cur ? top().cur : (top().cur = newState<QState>());
   }
   QState * state() {
      auto s = qobject_cast<QState*>(aState());
      if (!s) qFatal("Stateful: Source state must be a QState.");
      return s;
   }
   QState * parent() {
      auto s = qobject_cast<QState*>((m_levels.end()-2)->cur);
      if (!s) qFatal("Stateful: Parent state must be a QState.");
      return s;
   }
   QAbstractState * next() const { return top().next; }
   QAbstractState * failure() const { return top().failure; }
   template <typename T> QAbstractState * newState() {
      return top().cur = new T(parent());
   }
   void flush() {
      while (! m_ops.isEmpty()) {
         //qDebug() << "ops has" << m_ops.count();
         auto it = m_ops.end() - 1;
         int n = 0;
         while (! ((*it)->m_flags & OpBase::IsNew) && it != m_ops.begin()) { --it; n++; }
         top().cur = nullptr;
         (*it)->make(*this);
         auto successor = top().cur;
         qDebug() << "flush,successor" << successor << n;
         while (n--) {
            auto op = m_ops.pop();
            op->make(*this);
            qDebug() << "flush" << top().cur;
         }
         m_ops.pop();
         //qDebug() << "ops has at end" << m_ops.count();
         Q_ASSERT(successor == aState());
         top().next = top().cur;
      }
   }
};

template <class D> class Op : public OpBase {
   friend class Stateful;
protected:
   QString m_name;
   void make(Stateful & s) Q_DECL_OVERRIDE {
      if (!m_name.isEmpty()) s.aState()->setObjectName(m_name);
   }
   Op(int flags = 0) : OpBase(static_cast<Flags>(flags)) {}
public:
   D & s(const char * name) { m_name = name; return static_cast<D&>(*this); }
};

//

struct ADevice : Param {
   QPointer<QIODevice> device;
   static int id() { return 1; }
   explicit ADevice(QIODevice * device) : Param(id()), device(device) {}
};

class Final : public Op<Final> {
   bool m_isFailure { false };
protected:
   void make(Stateful & s) Q_DECL_OVERRIDE {
      s.newState<QFinalState>();
      Op::make(s);
   }
public:
   Final() : Op(MustBeNew) {}
   Final & failure() { m_flags |= IsFailure; }
};

class Send : public Op<Send> {
   QPointer<QIODevice*> m_dev;
   QByteArray m_data;
protected:
   void make(Stateful &s) Q_DECL_OVERRIDE {
      Op::make(s);
      auto p = s.param<ADevice>();
      Q_ASSERT(p);
      auto dev = p->device;
      auto data = m_data;
      QObject::connect(s.state(), &QState::entered, dev, [dev, data]{
         dev->write(data);
      });
   }
public:
   Send(const QByteArray & data) : m_data(data) {}
};

class Delay : public Op<Delay> {
   int m_delay;
   QAbstractState * m_dst { nullptr };
protected:
   void make(Stateful &s) Q_DECL_OVERRIDE {
      Op::make(s);
      if (!m_dst) m_dst = s.next();
      if (!m_dst) qFatal("Delay: No destination state");
      auto timer = new QTimer(s.state());
      timer->setSingleShot(true);
      timer->setInterval(m_delay);
      QObject::connect(s.state(), &QState::entered, timer, static_cast<void (QTimer::*)()>(&QTimer::start));
      QObject::connect(s.state(), &QState::exited,  timer, &QTimer::stop);
      s.state()->addTransition(timer, SIGNAL(timeout()), m_dst);
   }
public:
   Delay(int delay, QAbstractState * dst = nullptr) : Op(NeedsNext), m_delay(delay), m_dst(dst) {}
};

class Expect : public Op<Expect> {
   QByteArray m_data;
   QAbstractState * m_dst;
   int m_timeout;
   QAbstractState * m_timeoutDst;
protected:
   void make(Stateful &s) Q_DECL_OVERRIDE {
      Op::make(s);
      auto p = s.param<ADevice>();
      Q_ASSERT(p);
      auto dev = p->device;
      auto data = m_data;
      if (!m_dst) m_dst = s.next();
      addTransition(s.state(), m_dst, dev, SIGNAL(readyRead()), [dev, data]{
         return hasLine(dev, data);
      });
      if (! m_timeout) return;
      if (!m_timeoutDst) m_timeoutDst = s.failure();
      //s | Delay(m_timeout, m_timeoutDst);
   }
public:
   Expect(const QByteArray & data, int timeout = 0, QAbstractState * timeoutDst = nullptr) :
      Expect(data, 0, timeout, timeoutDst) {}
   Expect(const QByteArray & data, QAbstractState * dst, int timeout = 0, QAbstractState * timeoutDst = nullptr) :
      m_data(data), m_dst(dst), m_timeout(timeout), m_timeoutDst(timeoutDst)
   {
      if (!dst) m_flags |= NeedsNext;
      if (m_timeout && !m_timeoutDst) m_flags |= NeedsFailure;
   }
};

//

class Device : public QObject {
   Q_OBJECT
   Q_PROPERTY (bool running READ isRunning NOTIFY runningChanged)
   AppPipe m_dev { nullptr, QIODevice::ReadWrite, this };
   QStateMachine m_mach { this };
   QState      s_init { &m_mach },
               s_booting { &m_mach },
               s_firmware { &m_mach };
   QFinalState s_loaded { &m_mach };
public:
   Device(QObject * parent = 0) : QObject(parent) {
      s_init.setObjectName("s_init");
      s_booting.setObjectName("s_booting");
      s_firmware.setObjectName("s_firmware");
      s_loaded.setObjectName("s_loaded");
      for (auto state : m_mach.findChildren<QAbstractState*>())
         connect(state, &QState::entered, this, [this, state]{
            emit stateChanged(state->objectName());
         });
      connect(&m_mach, &QStateMachine::runningChanged, this, &Device::runningChanged);
      m_mach.setInitialState(&s_init);

      expect(&s_init, &m_dev, "boot", &s_booting);
      delay (&s_booting, 500, &s_firmware);
      send  (&s_firmware, &m_dev, "boot successful\n");
      expect(&s_firmware, &m_dev, ":00000001FF", &s_loaded);
      send  (&s_loaded,   &m_dev, "load successful\n");
   }
   Q_SLOT void start() { m_mach.start(); }
   Q_SLOT void stop() { m_mach.stop(); }
   Q_SIGNAL void stateChanged(const QString &);
   Q_SIGNAL void runningChanged(bool);
   bool isRunning() const { return m_mach.isRunning(); }
   AppPipe & pipe() { return m_dev; }
};

class Programmer : public QObject {
   Q_OBJECT
   Q_PROPERTY (bool running READ isRunning NOTIFY runningChanged)
   AppPipe m_port { nullptr, QIODevice::ReadWrite, this };
   QStateMachine m_mach { this };
   QState      s_boot { &m_mach },
               s_send { &m_mach };
   QFinalState s_ok { &m_mach },
               s_failed { &m_mach };
public:
   Programmer(QObject * parent = 0) : QObject(parent) {
      Stateful s{&m_mach};
      s * ADevice(&m_port);
      s + Send  ("boot\n") .s("s_boot");
      s | Expect("boot successful", 1000);
      s + Send  ("HULLOTHERE\n:00000001FF\n") .s("s_send");
      s | Expect("load successful", 1000);
      s + Final () .s("s_ok");
      s + Final ().failure() .s("s_failed");
      s.flush();
      auto initial = m_mach.findChild<QState*>("s_boot");
      m_mach.setInitialState(initial);

      for (auto state : m_mach.findChildren<QAbstractState*>())
         connect(state, &QState::entered, this, [this, state]{
            emit stateChanged(state->objectName());
         });
      connect(&m_mach, &QStateMachine::runningChanged, this, &Programmer::runningChanged);
   }
   Q_SLOT void start() { m_mach.start(); }
   Q_SIGNAL void runningChanged(bool);
   Q_SIGNAL void stateChanged(const QString &);
   bool isRunning() const { return m_mach.isRunning(); }
   AppPipe & pipe() { return m_port; }
};

struct Thread : public QThread {
   ~Thread() { quit(); wait(); }
};

static QString formatData(const char * prefix, const char * color, const QByteArray & data) {
   auto text = QString::fromLatin1(data).toHtmlEscaped();
   if (text.endsWith('\n')) text.truncate(text.size() - 1);
   text.replace(QLatin1Char('\n'), QString::fromLatin1("<br/>%1").arg(QLatin1String(prefix)));
   return QString::fromLatin1("<font color=\"%1\">%2 %3</font><br/>")
         .arg(QLatin1String(color)).arg(QLatin1String(prefix)).arg(text);
}

int main(int argc, char ** argv) {
   using Q = QObject;
   QApplication app{argc, argv};
   Thread devThread;
   Device dev;
   Programmer prog;
   if (false) {
      dev.moveToThread(&devThread);
      devThread.start();
   }

   QWidget w;
   QGridLayout grid{&w};
   QTextBrowser comms;
   QPushButton devStart{"Start Device"}, devStop{"Stop Device"},
               progStart{"Start Programmer"};
   QLabel devState, progState;
   grid.addWidget(&comms, 0, 0, 1, 3);
   grid.addWidget(&devState, 1, 0, 1, 2);
   grid.addWidget(&progState, 1, 2);
   grid.addWidget(&devStart, 2, 0);
   grid.addWidget(&devStop, 2, 1);
   grid.addWidget(&progStart, 2, 2);
   devStop.setDisabled(true);
   w.show();

   dev.pipe().addOther(&prog.pipe());
   prog.pipe().addOther(&dev.pipe());
   Q::connect(&prog.pipe(), &AppPipe::hasOutgoing, &comms, [&](const QByteArray & data){
      comms.append(formatData("&gt;", "blue", data));
   });
   Q::connect(&prog.pipe(), &AppPipe::hasIncoming, &comms, [&](const QByteArray & data){
      comms.append(formatData("&lt;", "green", data));
   });
   Q::connect(&devStart, &QPushButton::clicked, &dev, &Device::start);
   Q::connect(&devStop, &QPushButton::clicked, &dev, &Device::stop);
   Q::connect(&dev, &Device::runningChanged, &devStart, &QPushButton::setDisabled);
   Q::connect(&dev, &Device::runningChanged, &devStop, &QPushButton::setEnabled);
   Q::connect(&dev, &Device::stateChanged, &devState, &QLabel::setText);
   Q::connect(&progStart, &QPushButton::clicked, &prog, &Programmer::start);
   Q::connect(&prog, &Programmer::runningChanged, &progStart, &QPushButton::setDisabled);
   Q::connect(&prog, &Programmer::stateChanged, &progState, &QLabel::setText);
   return app.exec();
}

#include "main.moc"
