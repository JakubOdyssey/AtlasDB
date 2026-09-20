# Documentation diagrams

The documentation embeds static SVG images so viewing it does not require a
Mermaid renderer or JavaScript. Each SVG has an editable Mermaid source with
the same name and the `.mmd` extension. These sources preserve the diagram
labels and connections from the documentation.

The exports use Mermaid 11.4.1 and [mermaid-config.json](mermaid-config.json).
They contain native SVG text and paths, with a white background and no HTML
labels, scripts, external fonts or remote resources. The images were checked
locally as ordinary image elements with JavaScript and network access disabled.

To update a diagram, edit its `.mmd` source and export the matching SVG with the
configuration above. Keep `htmlLabels` disabled, retain the white background,
and check the SVG as an image rather than only as inline HTML. Keep both files
together; the exported SVG is an intentional documentation asset.

| Diagram | Editable source | Static image |
|---|---|---|
| Architecture overview | [Source](architecture-overview.mmd) | [SVG](architecture-overview.svg) |
| Tree overview | [Source](btree-overview.mmd) | [SVG](btree-overview.svg) |
| Recovery overview | [Source](recovery-overview.mmd) | [SVG](recovery-overview.svg) |
| Architecture and ownership | [Source](architecture.mmd) | [SVG](architecture.svg) |
| Tree invariants | [Source](btree.mmd) | [SVG](btree.svg) |
| Transaction states | [Source](transactions.mmd) | [SVG](transactions.svg) |
| WAL and recovery | [Source](wal-and-recovery.mmd) | [SVG](wal-and-recovery.svg) |
