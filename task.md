# RAF-TDD Spec v2

In this project, you need to design and implement a new file archive format called **Record Archive Format (RAF)**.

---

## Background

### Why do we need RAF?

Many systems in a trading or data platform team start with the same basic problem: we receive a continuous stream of events, and we need to store those events so they can be replayed or inspected later. These events may be exchange market data messages, order events, trade events, risk events, strategy decisions and so on. They usually arrive in time order. We need to write them to the storage in a fast and durable way. And later we want to read the data with high performance and reliability, too.

We need a specific data format that can satisfy our needs above. And we call this data format "Record Archive Format (RAF)".

The hard part is that this storage path must be both fast and resilient. During busy market periods, a collector may receive a large burst of small messages, and any slow write path can become backpressure for the upstream service. During research or replay, the same archive may need to be scanned at high throughput, or accessed randomly to inspect a specific sequence number. A format that is correct but slow will make data ingestion fragile and offline analysis expensive.

At the same time, the archive must be resilient. Machines restart, processes crash, disks return short reads or failed writes, and deployments may stop a service while it is writing. A useful archive format should preserve the valid prefix, detect corrupted records, and make recovery behavior explicit. In other words, RAF is not only about storing bytes correctly; it is about building a file format that keeps its performance characteristics while still behaving predictably under failure.

### Who uses RAF?

Orderbook reconstruction is one important user of this storage format. Exchanges usually do not publish a complete order book at every moment. Instead, they publish incremental messages such as order additions, cancellations, trades, snapshots, or book updates. An orderbook reconstruction pipeline replays these messages and rebuilds the state of the book at each point in time.

Other than the order book reconstruction use case we discussed above, there are many other users that rely on this data format:

- **Backtesting** needs to replay historical events in the same order as they were collected.
- **Audit tools** need to prove which raw inputs were available before a decision was made.
- **Incident debugging** needs fast random access to records near a suspicious timestamp or sequence number.
- **Data conversion jobs** need to scan large files and produce derived formats for research.

In real production systems, this component would likely be one layer inside a larger pipeline:

```
Exchange / service / strategy
            |
            v
   event normalization
            |
            v
      RecordWriter
            |
            v
  record files
            |
            v
      RecordReader
            |
            +--> sequential replay for orderbook reconstruction
            +--> random access for debugging and audit
            +--> batch scan for research data conversion
            +--> recovery after crash or partial write
```

### How this task relates to real data pipelines?

This task is an intentionally simplified version of that storage problem. It does not require schema evolution, compression, distributed replication, timestamp indexing, or integration with a real exchange feed. The input is simply a binary string, and the output is an index that can later be used for reading. However, the core storage questions are still real: how to lay out records on disk, how to build an index, how to detect corruption, how to recover after a partial write, how to define thread safety, and how to measure the performance under random and sequential workloads.

You are asked to design a data format that offers the three key properties below:

| Property                        | Why It Matters                                                                                     |
| ------------------------------- | -------------------------------------------------------------------------------------------------- |
| **Fast append writes**          | The archive should not become backpressure during data collection.                                 |
| **Sequential and random reads** | Replay jobs scan records in order, while debugging often needs direct access to a specific record. |
| **Reliable recovery**           | A crash near the file tail should not invalidate the whole archive.                                |

---

## Project Goals

In this project, you need to carry out the tasks below one by one:

1. **Design:** Design the RAF data format.
2. **Test:** Follow test driven development practice by first defining comprehensive tests.
3. **Implement:** Implement your own design in C++ with robust code, and pass all tests.
4. **Optimize:** Measure and improve I/O performance.

You are also expected to manage the project timeline. Before implementation, define milestones, expected deliverables, and risks. During the project, update the plan when major design decisions change.

---

## Design

Understand the requirements, design the corresponding classes and interfaces, and complete the design documentation.

1. You should design file format, write procedure, read procedure, interfaces, how to handle failure and ensure data integrity, performance expectation, etc.
2. You could use this as a template: [\[Template\] Design Doc](./raf-design-template.md) or create your own document.

In this task, you need to design and implement `RecordWriter` and `RecordReader`, where:

1. **RecordWriter:** Supports **sequential** writes to files, and returns the corresponding index(es) after writing the value(s).
2. **RecordReader:** Supports **sequential and random** reading of files, where sequential reading is in **iterator mode** and random reading is reading using the index(es) returned after writing.

```cpp
typedef int INDEX_TYPE;
class RecordWriter
{
public:
    /**
     * Open a record archive file.
     */
    RecordWriter(const std::string &filepath);
    /**
     * Append one binary value and return its index.
     */
    INDEX_TYPE append(const std::string &value);
    /**
     * Append multiple binary values and return their indexes.
     */
    std::vector<INDEX_TYPE> appendBatch(const std::vector<std::string> &values);
    /**
     * Close the writer. Writing after close must fail.
     */
    void close();
};
```

> **Note:** We only provide the design of the RecordWriter interface; please refer to the interface to design the RecordReader interface.

### Additional requirements

**Failure handling:**

The record archive reader/writer should properly handle failovers or clearly claim the result of failures. For example:

1. File corruption when crashes happen.
2. Handle opening a corrupted file.

**Thread safety:**

Your API should be thread-safe (you can use multithreading or async IO to speed up). And you need to clearly state the semantics under concurrency. For example, what are the behaviors when:

- There are multiple readers and writers operating on the same file in one process.
- There are multiple processes operating on the same file.
- ...

---

## Test

In this project we ask you to follow a **Test-Driven Development** process. You need to complete a core set of test cases **before starting your code implementation**.

We recommend that you use this **TDD (Test-driven Development)** thinking mode, which can not only verify your interface design, but also help you ensure the correctness of the program during iterative development.

If you are not familiar with the various types of test cases and TDD, you can refer to:

1. [Software Testing Types: A Comprehensive Overview](https://medium.com/@abhaykhs/software-testing-types-a-comprehensive-overview-ae99cc311586)
2. [How to use negative testing to create more resilient software](https://qase.io/blog/how-to-use-negative-testing-to-create-more-resilient-software/)
3. [Introduction to Unit Testing and Test-Driven Development in C++](https://medium.com/@AlexanderObregon/introduction-to-unit-testing-and-test-driven-development-in-c-1a825a00b1b4)

Write test cases including **unit tests, integration tests, and stress tests**.

1. **Unit tests:** Write test cases for each RecordReader and RecordWriter interface.
2. **Integration tests:** Integrate RecordReader and RecordWriter modules to read and write the same file.
3. **Stress tests:** Perform highly concurrent read and write operations on files, such as:
   - Hold multiple RecordReaders to read the same file concurrently.
   - Hold multiple RecordWriters to write different files concurrently.
   - ... (You can design more stress tests based on your thread-safety model)
4. For each of the above tests, please write as detailed (happy path, sad path) test cases as possible to cover various situations.

### Stress Tests with a Baseline

In order to find out reasonable expectations on the performance, it is important to establish a performance baseline before you stress test your own implementation. You will need to first benchmark your working environment (the disk you run your RAF on). You can use [FIO](https://fio.readthedocs.io/en/latest/) to benchmark that environment first, and gain basic understandings about the read and write throughput/IOPS characteristics. Then you should define performance goals for your implementation.

---

## Implement

Once you have defined and implemented a core set of tests, you may move forward to implement your core logic – the RecordReader and RecordWriter. The process will be iterative and will probably look like:

1. Run a test case and the test case does not pass
2. Implement the corresponding functionality in the RecordReader or RecordWriter
3. Run the test case again and it passes
4. Commit the change. And go back to step 1.

Note that most probably, new test cases may come to your mind as you implement your RecordReader and RecordWriter. This is natural. However, TDD style requires you to try your best to define a test case on a non-existent behavior, not implement the behavior first then test.

At the end of this implementation stage, you should have a solid set of test cases, covering both the success cases and failure cases of your RecordReader and RecordWriter implementation.

---

## Optimize

Benchmark your own implementation of the RAF data format, by testing TPS/throughput of 4KB/1MB random/sequential read/write and complete a table that looks like:

| Record Size | Concurrency | READ — Random  | READ — Sequential | WRITE — Sequential |
| ----------- | ----------- | -------------- | ----------------- | ------------------ |
| 4KB         | 1           | a TPS / b MB/s |                   |                    |
|             | N           |                |                   |                    |
| 1MB         | 1           |                |                   |                    |
|             | N           |                |                   |                    |

You need to compare your performance with the performance baseline with FIO on your own disk. In previous steps you have defined performance goals, based on FIO benchmark on your own disk. In this step, you need to optimize your own implementations, until you achieve those goals. It typically takes rounds of iterations before getting there.

---

## Report

In the end of the project, besides your code and your design doc, you will also need to submit two reports:

1. **Test report:** Describe the test strategy, covered scenarios, missing scenarios, and evidence that the implementation behaves correctly.
2. **Performance report:** Show benchmark results across optimization rounds. For each optimization, include the before/after numbers and explain why the result changed.