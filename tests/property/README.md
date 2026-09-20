# Model-based tests

`deterministic_transaction_model` runs three published seeds against `std::map`.
It compares ordered contents and checks every allocated page after every commit
or rollback, then periodically reopens the file. The oracle models transaction
boundaries, including read-your-writes and aborted mutations.

`random_slotted_page_operations` independently models slot identity and byte
contents across allocation, fragmentation, deletion and compaction.

Seeds are printed before execution. Assertions remain enabled in Release builds.
