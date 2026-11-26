#pragma once
#ifndef ALGORITHMTHREAD_H
#define ALGORITHMTHREAD_H

#include <QThread>
#include <atomic>

class map_check_task_manager;

class AlgorithmThread : public QThread
{
    Q_OBJECT
public:
    explicit AlgorithmThread(map_check_task_manager* manager, QObject* parent = nullptr);
    ~AlgorithmThread() override;

    void requestPause();
    void requestResume();
    void requestStop();

    bool isPaused() const { return m_paused.load(); }
    bool isStopped() const { return m_stopped.load(); }

signals:
    void progressChanged(int value);
    void finished();

protected:
    void run() override;

private:
    map_check_task_manager* m_manager;
    std::atomic<bool> m_paused;
    std::atomic<bool> m_stopped;
};

#endif // ALGORITHMTHREAD_H
