"""Deterministic checkbox-tree rows derived only from discovered preset files."""
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Node:
    id: str
    parent: int
    label: str
    location: str = ''
    source: object = None

    @property
    def display_label(self):
        return self.source.path.stem if self.source else self.label


def tree(libraries, selected):
    nodes = []
    for library in libraries:
        if library.path not in selected:
            continue
        root = len(nodes)
        nodes.append(Node(str(root), -1, library.product, str(library.path)))
        folders = {(): root}
        def folder_first(source):
            parts = source.path.relative_to(library.path).parts
            # At every level, folders precede loose files regardless of name.
            return tuple((i == len(parts) - 1, part.casefold(), part)
                         for i, part in enumerate(parts))

        for source in sorted(library.sources, key=folder_first):
            parts = source.path.relative_to(library.path).parts
            for depth in range(1, len(parts)):
                key = parts[:depth]
                if key not in folders:
                    index = len(nodes)
                    folders[key] = index
                    nodes.append(Node(str(index), folders[key[:-1]], key[-1]))
            index = len(nodes)
            nodes.append(Node(str(index), folders[parts[:-1]], parts[-1], source=source))
    return nodes
