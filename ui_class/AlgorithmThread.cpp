#include "AlgorithmThread.h"
#include "map_check_task_manager.h"

AlgorithmThread::AlgorithmThread(map_check_task_manager* manager, QObject* parent)
    : QThread(parent),
    m_manager(manager),
    m_paused(false),
    m_stopped(false)
{}

AlgorithmThread::~AlgorithmThread()
{
    requestStop();
    wait();
}

void AlgorithmThread::requestPause() { m_paused.store(true); }
void AlgorithmThread::requestResume() { m_paused.store(false); }
void AlgorithmThread::requestStop() { m_stopped.store(true); m_paused.store(false); }

void AlgorithmThread::run()
{
    if (!m_manager) return;
    m_manager->executeAlgorithms(m_stopped, m_paused, this);
    emit finished();
}
