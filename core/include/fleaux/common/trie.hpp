#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fleaux::common {

// A simple prefix trie for string symbol storage and completion lookups.
// All operations are O(m) where m is the length of the query string.
class Trie {
public:
  // Inserts a word into the trie.
  void insert(const std::string_view word) {
    Node* current = &root_;
    for (const char ch : word) {
      current = &current->children[ch];
    }
    current->is_terminal = true;
  }

  // Returns all words stored in the trie that begin with the given prefix.
  // The prefix itself is included if it was directly inserted.
  [[nodiscard]] auto completions(const std::string_view prefix) const -> std::vector<std::string> {
    const Node* current = &root_;
    for (const char ch : prefix) {
      const auto it = current->children.find(ch);
      if (it == current->children.end()) { return {}; }
      current = &it->second;
    }

    std::vector<std::string> results;
    std::string accumulator{prefix};
    collect_words(*current, accumulator, results);
    return results;
  }

  // Returns true if no symbols have been loaded.
  [[nodiscard]] auto empty() const -> bool {
    return root_.children.empty() && !root_.is_terminal;
  }

  // Removes all inserted symbols.
  void clear() { root_ = Node{}; }

private:
  struct Node {
    std::unordered_map<char, Node> children;
    bool is_terminal = false;
  };

  static void collect_words(const Node& node, std::string& current, std::vector<std::string>& out) {
    if (node.is_terminal) { out.push_back(current); }
    for (const auto& [ch, child] : node.children) {
      current.push_back(ch);
      collect_words(child, current, out);
      current.pop_back();
    }
  }

  Node root_;
};

}  // namespace fleaux::common
