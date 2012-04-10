/*
 * Copyright (C) 2012  Politecnico di Milano
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BBQUE_METRICS_COLLECTOR_H_
#define BBQUE_METRICS_COLLECTOR_H_

#include "bbque/plugins/logger.h"
#include "bbque/utils/timer.h"

#include <map>
#include <vector>
#include <memory>
#include <mutex>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/moment.hpp>
#include <boost/accumulators/statistics/variance.hpp>

using namespace boost::accumulators;
using bbque::plugins::LoggerIF;
using bbque::utils::Timer;

namespace bbque { namespace utils {

/**
 * @brief Interface to collect statistics on different metrics
 *
 * Metrics allows to account and analyze different system parameters. This is
 * a base class which provides a centralized repository for system metrics
 * which could be dynamically defined, updated and queryed.
 */
class MetricsCollector {

public:

	/**
	 * @brief Return codes generated by methods of this class
	 */
	typedef enum {
		OK,
		DUPLICATE,
		UNSUPPORTED,
		UNKNOWEN
	} ExitCode_t;

	/**
	 * @brief The supported kinds of metrics
	 */
	typedef enum {
		/** A generic "u64" event counter */
		COUNTER = 0,
		/** A generic "u64" value which could be increased/decreased */
		VALUE,
		/** A generic "double" sample for statistical computations */
		SAMPLE,
		/** A generic "time" sample for periods computations */
		PERIOD,

		/** The number of metrics classes */
		CLASSES_COUNT // This MUST be the last value

	} MetricClass_t;

	/** The handler of a registered metrics */
	typedef size_t MetricHandler_t;

	/**
	 * @brief A collection of metrics to be registerd
	 *
	 * A client module could register a set of metrics by declaring an array
	 * of this structures, filled with proper values describing each single
	 * metrics of interest, and passing this to the provided registration
	 * method.
	 */
	typedef struct MetricsCollection {
		/** The name of the metric */
		const char *name;
		/** A textual description of the metric */
		const char *desc;

		/** The class of this metirc */
		MetricClass_t mc;

		/** The submetrics count */
		uint8_t sm_count;
		/** The sumbetrics descriptions */
		const char **sm_desc;

		/** A metric handler returned by the registration method */
		MetricHandler_t mh;

	} MetricsCollection_t;

	/**
	 * @brief The base class for a registered metric
	 *
	 * This is the data structure representing the maximum set of information
	 * common to all metric classes. Specific metrics are specified by
	 * deriving this class.
	 */
	class Metric {
	public:
		/** The name of the metric */
		const char *name;
		/** A textual description of the metric */
		const char *desc;

		/** The class of this metirc */
		MetricClass_t mc;

		/** The number of registered submetrics */
		uint8_t sm_count;
		/** A textual description of the sub-metrics */
		const char **sm_desc;

		/** Mutex protecting concurrent access to statistical data */
		std::mutex mtx;

		Metric(const char *name, const char *desc, MetricClass_t mc,
				uint8_t sm_count = 0, const char **sm_desc = NULL) :
			name(name), desc(desc), mc(mc),
			sm_count(sm_count), sm_desc(sm_desc) {
		};

		bool HasSubmetrics() const {
			return (sm_count != 0);
		}
	};

	/** A pointer to a (base class) registered metrics */
	typedef std::shared_ptr<Metric> pMetric_t;

	/**
	 * @brief A counting metric
	 *
	 * This is a simple metric which could be used to count events. Indeed
	 * this metrics supports only the "increment" operation.
	 */
	class CounterMetric : public Metric {
	public:
		uint64_t cnt;
		std::vector<uint64_t> sm_cnt;

		CounterMetric(const char *name, const char *desc,
				uint8_t sm_count = 0, const char **sm_desc = NULL);
	};

	/**
	 * @brief A value accounting metrics
	 *
	 * This is a simple metric which could be used to keep track of a certain
	 * value (e.g. amount of memory, total time spent) which chould both
	 * increment or decrement of a specified value. The metrics keep track
	 * also of the "maximum" and "minimum" value for the metric.
	 */
	class ValueMetric : public Metric {
	public:
		/** The current value */
		uint64_t value;
		std::vector<uint64_t> sm_value;

		/** Statistics on Value */
		typedef accumulator_set<uint64_t,
			stats<tag::count, tag::min, tag::max>> statMetric_t;
		typedef std::shared_ptr<statMetric_t> pStatMetric_t;
		pStatMetric_t pstat;
		std::vector<pStatMetric_t> sm_pstat;

		ValueMetric(const char *name, const char *desc,
				uint8_t sm_count = 0, const char **sm_desc = NULL);

	};

	/**
	 * @brief A statistic collection metrics
	 *
	 * This is a metrics which could be useed to compute a statistic on a set
	 * of samples. Indeed, this metric support only the sample addition
	 * operations: each time a new sample is added to the metric a set of
	 * statistics are updated. So far the supported statistics are:
	 * minumum, maximum, mead and variance.<br>
	 * Mean and variance are computed on the <i>complete population</i>, i.e.
	 * considering all the samples collected so far.
	 */
	class SamplesMetric : public Metric {
	public:
		/** Statistics on collected Samples */
		typedef accumulator_set<double,
			stats<tag::min, tag::max, tag::variance>> statMetric_t;
		typedef std::shared_ptr<statMetric_t> pStatMetric_t;
		pStatMetric_t pstat;
		std::vector<pStatMetric_t> sm_pstat;

		SamplesMetric(const char *name, const char *desc,
				uint8_t sm_count = 0, const char **sm_desc = NULL);

	};

	/**
	 * @brief A period/frequency accounting metrics
	 *
	 * This is a simple metric which could be used to compute the
	 * period/frequency of a certain event, by simply collecting a set of
	 * measured timeframes. The metrics keep track also of the "maximum" and
	 * "minimum" value for time intervals.
	 */
	class PeriodMetric : public Metric {
	public:
		/** The timer used for period computation */
		Timer period_tmr;
		std::vector<Timer> sm_period_tmr;

		/** Statistics on Value */
		typedef accumulator_set<double,
			stats<tag::min, tag::max, tag::variance>> statMetric_t;
		typedef std::shared_ptr<statMetric_t> pStatMetric_t;
		pStatMetric_t pstat;
		std::vector<pStatMetric_t> sm_pstat;

		PeriodMetric(const char *name, const char *desc,
				uint8_t sm_count = 0, const char **sm_desc = NULL);

	};

	/**
	 * @brief Get a reference to the metrics collector
	 * The MetricsCollector is a singleton class providing the glue logic for
	 * the Barbeque metrics collection, management and query.
	 *
	 * @return  a reference to the MetricsCollector singleton instance
	 */
	static MetricsCollector & GetInstance();

	/**
	 * @brief  Clean-up the metrics collector data structures.
	 */
	~MetricsCollector();

	/**
	 * @brief Register a new system metric
	 *
	 * Once a new metric is needed it should be first registered to the
	 * MetricsCollector class by using this method. The registeration requires
	 * a metric name, which is used to uniquely identify it, as well as the
	 * metric class. This method returns, on success, a "metrics handler"
	 * which is required by many other access/modification methods.
	 * The 'count' parameters defines the number of such metrics to be
	 * instantiated, if count>1, that an index is appended to the metric name
	 * and 'count' metrics are actually registered.
	 */
	ExitCode_t Register(const char *name,
			const char *desc,
			MetricClass_t mc,
			MetricHandler_t & mh,
			uint8_t sm_count = 0,
			const char **sm_desc = NULL);

	/**
	 * @brief Register the sepcified set of metrics
	 *
	 * A client module could register a set of metrics by using this method
	 * which requires a pointer to a MetricsCollection, i.e. a pre-loaded
	 * vector of metrics descriptors.
	 */
	ExitCode_t Register(MetricsCollection_t *mc, uint8_t m_count);

	/**
	 * @brief Increase the specified counter metric
	 *
	 * This method is reserved to metrics of COUNT class and allows to
	 * increment the counter by the specified amount (by default 1).
	 */
	ExitCode_t Count(MetricHandler_t mh, uint64_t amount = 1,
			uint8_t sm_idx = 0);

	/**
	 * @brief Add the specified amount to a value metric
	 *
	 * This method is reserved to metrics of VALUE class and allows to augment
	 * the current metric value by the specified amount.
	 */
	ExitCode_t Add(MetricHandler_t mh, double amount, uint8_t sm_idx = 0);

	/**
	 * @brief Subtract the specified amount to a value metric
	 *
	 * This method is reserved to metrics of VALUE class and allows to
	 * decrease the current metric value by the specified amount.
	 */
	ExitCode_t Remove(MetricHandler_t mh, double amount, uint8_t sm_idx = 0);

	/**
	 * @brief Reset the specified value metric
	 *
	 * This method is reserved to metrics of VALUE class and allows to reset
	 * the current value of the specified metric.
	 */
	ExitCode_t Reset(MetricHandler_t mh, uint8_t sm_idx = 0);

	/**
	 * @brief Add a new sample to a SAMPLE metric
	 *
	 * This method is reserved to metrics of SAMPLE class and allows to collect
	 * one more sample, thus updating the metrics statistics.
	 */
	ExitCode_t AddSample(MetricHandler_t mh, double sample, uint8_t sm_idx = 0);

	/**
	 * @brief Add a new time sample to a PERIDO metric
	 *
	 * This method is reserved to metrics of SAMPLE class and allows to collect
	 * one more sample, thus updating the metrics statistics.
	 */
	ExitCode_t PeriodSample(MetricHandler_t mh, double & last_period,
			uint8_t sm_idx = 0);

	/**
	 * @brief Dump on screen a report of all the registered metrics
	 *
	 * This method is intended mainly dor debugging and allows to report on
	 * screen the current values for all the registered metrics.
	 */
	void DumpMetrics();

private:

	/** A map of metrics handlers on correpsonding registered metrics */
	typedef std::map<MetricHandler_t, pMetric_t> MetricsMap_t;

	/** An entry of the metrics maps */
	typedef std::pair<MetricHandler_t, pMetric_t> MetricsMapEntry_t;

	/** A set of metrics maps grouped by class */
	typedef MetricsMap_t MetricsVec_t[CLASSES_COUNT];

	/** A string representation of metric classes */
	static const char *metricClassName[CLASSES_COUNT];

	template<typename T>
	class MetricStats {
	public:
		T min, max, avg, var;
		char name[21], desc[64];
		MetricStats() :
			min(0), max(0), avg(0), var(0) {
			name[0] = 0;
			desc[0] = 0;
		}
	};


	/**
	 * @brief The logger to use.
	 */
	LoggerIF *logger;

	/**
	 * @brief Map of all registered metrics
	 */
	MetricsMap_t metricsMap;

	/**
	 * @brief The mutex protecting access to metrics maps
	 */
	std::mutex metrics_mtx;

	/**
	 * @brief Vector of registered metrics grouped by class
	 */
	MetricsVec_t metricsVec;

	/**
	 * @brief Build a new MetricsCollector
	 */
	MetricsCollector();

	/**
	 * @brief Get the handler of the metric specified by name
	 *
	 * Return the handler of a metrics with the specified name.
	 */
	MetricHandler_t GetHandler(const char *name);

	/**
	 * @brief Get a reference to the registered metrics with specified handler
	 *
	 * Given the handler of a metrics, this method return a reference to its
	 * base class, or an empty pointer if the metric has not yet been
	 * registered.
	 */
	pMetric_t GetMetric(MetricHandler_t hdlr);

	/**
	 * @brief Get a reference to the registered metrics with specified name
	 *
	 * Given the name of a metrics, this method return a reference to its
	 * base class, or an empty pointer if the metric has not yet been
	 * registered.
	 */
	pMetric_t GetMetric(const char *name);

	/**
	 * @brief Update a metrics of VALUE class of the specified amout.
	 *
	 * The specified amount is added/removed for the metrics, the metrics is
	 * reset if amount is ZERO.
	 */
	ExitCode_t UpdateValue(MetricHandler_t mh, double amount,
			uint8_t sm_idx = 0);


	/**
	 * @brief Dump the current value for a metric of class COUNT
	 */
	void DumpCounter(CounterMetric *m);

	/**
	 * @brief Dump the current value for a sub-metric of class COUNT
	 */
	void DumpCountSM(CounterMetric *m, uint8_t idx);


	/**
	 * @brief Dump the current value for a metric of class VALUE
	 */
	void DumpValue(ValueMetric *m);

	/**
	 * @brief Dump the current value for a sub-metric of class SAMPLE
	 */
	void DumpValueSM(ValueMetric *m, uint8_t idx, MetricStats<uint64_t> &ms);


	/**
	 * @brief Dump the current value for a metric of class SAMPLE
	 */
	void DumpSample(SamplesMetric *m);

	/**
	 * @brief Dump the current value for a sub-metric of class SAMPLE
	 */
	void DumpSampleSM(SamplesMetric *m, uint8_t idx, MetricStats<double> &ms);


	/**
	 * @brief Dump the current value for a metric of class PERIOD
	 */
	void DumpPeriod(PeriodMetric *m);

	/**
	 * @brief Dump the current value for a sub-metric of class PERIOD
	 */
	void DumpPeriodSM(PeriodMetric *m, uint8_t idx, MetricStats<double> &ms);

};

} // namespace utils

} // namespace bbque

#endif // BBQUE_METRICS_COLLECTOR_H_
