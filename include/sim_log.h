/**
 * @file sim_log.h
 * @brief Centralized logging utility for Sniper simulator
 *
 * Provides consistent, structured logging across all simulator components.
 * Supports debug levels (NONE, BASIC, DETAILED) via debug_config.h.
 */

#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <iostream>
#include <mutex>
#include "simulator.h"
#include "config.hpp"
#include "debug_config.h"

/**
 * @brief Singleton class for unified logging across all components
 */
class UnifiedLog {
private:
    std::ofstream m_file;
    std::mutex m_mutex;
    bool m_initialized;
    bool m_enabled;

    UnifiedLog() : m_initialized(false), m_enabled(false) {}

public:
    static UnifiedLog& instance() {
        static UnifiedLog inst;
        return inst;
    }

    void init() {
        if (m_initialized) return;
        m_initialized = true;

#if UNIFIED_LOG_ENABLED
        m_enabled = true;
        std::string path = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                          + "/" + UNIFIED_LOG_FILENAME;
        m_file.open(path);
        if (m_file.is_open()) {
            m_file << "=== UNIFIED DEBUG LOG ===" << std::endl;
            m_file << "All component logs in timing order" << std::endl;
            m_file << std::string(60, '=') << std::endl << std::endl;
        }
#endif
    }

    bool isEnabled() const { return m_enabled && m_file.is_open(); }

    void write(const std::string& component, int core_id, const std::string& message) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (core_id >= 0) {
            m_file << "[" << std::setw(12) << std::left << component
                   << "|C" << core_id << "] " << message << std::endl;
        } else {
            m_file << "[" << std::setw(15) << std::left << component << "] "
                   << message << std::endl;
        }
    }

    void writeWithTime(const std::string& component, int core_id, uint64_t sim_time_ns,
                       const std::string& message) {
        if (!isEnabled()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream prefix;
        prefix << "@" << std::setw(12) << sim_time_ns << "ns ";
        if (core_id >= 0) {
            prefix << "[" << std::setw(12) << std::left << component
                   << "|C" << core_id << "] ";
        } else {
            prefix << "[" << std::setw(15) << std::left << component << "] ";
        }
        m_file << prefix.str() << message << std::endl;
    }

    void flush() {
        if (isEnabled()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_file.flush();
        }
    }

    ~UnifiedLog() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    UnifiedLog(const UnifiedLog&) = delete;
    UnifiedLog& operator=(const UnifiedLog&) = delete;
};

class SimLog {
public:
    enum Level {
        LEVEL_NONE = 0,
        LEVEL_INFO = 1,
        LEVEL_DEBUG = 2,
        LEVEL_TRACE = 3
    };

private:
    std::ofstream m_file;
    std::string m_component;
    int m_core_id;
    int m_debug_level;
    bool m_enabled;

    std::string formatPrefix(Level level) {
        std::ostringstream oss;
        if (m_core_id >= 0) {
            oss << "[" << std::setw(12) << std::left << m_component
                << "|C" << m_core_id << "] ";
        } else {
            oss << "[" << std::setw(12) << std::left << m_component << "] ";
        }
        (void)level;
        return oss.str();
    }

    template<typename T>
    void appendArgs(std::ostringstream& oss, const T& arg) {
        oss << arg;
    }

    template<typename T, typename... Args>
    void appendArgs(std::ostringstream& oss, const T& first, const Args&... rest) {
        oss << first << " ";
        appendArgs(oss, rest...);
    }

    void writeToLogs(Level level, const std::string& message) {
        if (m_file.is_open())
            m_file << formatPrefix(level) << message << std::endl;
        UnifiedLog::instance().write(m_component, m_core_id, message);
    }

    void writeToLogsWithTime(Level level, uint64_t sim_time_ns, const std::string& message) {
        std::ostringstream oss;
        oss << "@" << std::setw(12) << sim_time_ns << "ns: " << message;
        if (m_file.is_open())
            m_file << formatPrefix(level) << oss.str() << std::endl;
        UnifiedLog::instance().writeWithTime(m_component, m_core_id, sim_time_ns, message);
    }

public:
    SimLog(const std::string& component, int core_id = -1, int debug_level = DEBUG_NONE)
        : m_component(component), m_core_id(core_id), m_debug_level(debug_level), m_enabled(true) {

        UnifiedLog::instance().init();

        if (debug_level == DEBUG_NONE) {
            m_enabled = false;
            return;
        }

        std::string filename = component;
        for (char& c : filename) {
            if (c == ' ') c = '_';
            c = std::tolower(c);
        }

        if (core_id >= 0) {
            filename += "." + std::to_string(core_id);
        }
        filename += ".log";

        std::string path = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                          + "/" + filename;
        m_file.open(path);
    }

    // Default constructor for use as member variable before initialization
    SimLog() : m_core_id(-1), m_debug_level(DEBUG_NONE), m_enabled(false) {}

    ~SimLog() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    bool isEnabled(Level level = LEVEL_DEBUG) const {
        if (!m_enabled) return false;
        if (level == LEVEL_INFO) return true;
        if (level == LEVEL_DEBUG && m_debug_level >= DEBUG_BASIC) return true;
        if (level == LEVEL_TRACE && m_debug_level >= DEBUG_DETAILED) return true;
        return false;
    }

    template<typename... Args>
    void info(const Args&... args) {
        if (!m_enabled) return;
        std::ostringstream oss;
        appendArgs(oss, args...);
        writeToLogs(LEVEL_INFO, oss.str());
    }

    template<typename... Args>
    void debug(const Args&... args) {
        if (!isEnabled(LEVEL_DEBUG)) return;
        std::ostringstream oss;
        appendArgs(oss, args...);
        writeToLogs(LEVEL_DEBUG, oss.str());
    }

    template<typename... Args>
    void trace(const Args&... args) {
        if (!isEnabled(LEVEL_TRACE)) return;
        std::ostringstream oss;
        appendArgs(oss, args...);
        writeToLogs(LEVEL_TRACE, oss.str());
    }

    void logAddress(Level level, const std::string& msg, uint64_t addr) {
        if (!isEnabled(level)) return;
        std::ostringstream oss;
        oss << msg << " 0x" << std::hex << addr << std::dec;
        writeToLogs(level, oss.str());
    }

    template<typename... Args>
    void logWithTime(Level level, uint64_t sim_time_ns, const Args&... args) {
        if (!isEnabled(level)) return;
        std::ostringstream oss;
        appendArgs(oss, args...);
        writeToLogsWithTime(level, sim_time_ns, oss.str());
    }

    void section(const std::string& title) {
        if (!m_enabled) return;
        if (m_file.is_open()) {
            m_file << "\n" << std::string(50, '-') << std::endl;
            m_file << "  " << title << std::endl;
            m_file << std::string(50, '-') << std::endl;
        }
        UnifiedLog::instance().write(m_component, m_core_id, "--- " + title + " ---");
    }

    template<typename... Args>
    void log(const Args&... args) { info(args...); }

    template<typename... Args>
    void detailed(const Args&... args) { trace(args...); }

    static std::string hex(uint64_t addr) {
        std::ostringstream oss;
        oss << "0x" << std::hex << addr << std::dec;
        return oss.str();
    }

    std::ofstream& stream() { return m_file; }
};

#define SIM_LOG_INFO(logger, ...) \
    do { if ((logger).isEnabled(SimLog::LEVEL_INFO)) (logger).info(__VA_ARGS__); } while(0)

#define SIM_LOG_DEBUG(logger, ...) \
    do { if ((logger).isEnabled(SimLog::LEVEL_DEBUG)) (logger).debug(__VA_ARGS__); } while(0)

#define SIM_LOG_TRACE(logger, ...) \
    do { if ((logger).isEnabled(SimLog::LEVEL_TRACE)) (logger).trace(__VA_ARGS__); } while(0)
