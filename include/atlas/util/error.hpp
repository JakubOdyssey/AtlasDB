#pragma once
#include <stdexcept>
namespace atlas {
struct Error : std::runtime_error {
    using runtime_error::runtime_error;
};
struct IoError : Error {
    using Error::Error;
};
struct CorruptionError : Error {
    using Error::Error;
};
struct InvalidDatabase : CorruptionError {
    using CorruptionError::CorruptionError;
};
struct PageNotFound : Error {
    using Error::Error;
};
struct TransactionError : Error {
    using Error::Error;
};
struct BusyError : TransactionError {
    using TransactionError::TransactionError;
};
struct CommitOutcomeUnknown : TransactionError {
    using TransactionError::TransactionError;
};
struct RecoveryError : CorruptionError {
    using CorruptionError::CorruptionError;
};
struct BufferPoolError : Error {
    using Error::Error;
};
struct LimitError : Error {
    using Error::Error;
};
} // namespace atlas
