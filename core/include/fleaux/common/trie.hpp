#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fleaux::common {

// A simple prefix trie for string symbol storage and completion lookups.
// Insert/lookup walk the query one character at a time and binary-search the
// current node's sorted child vector.
class Trie {
public:
  // Inserts a word into the trie.
  void insert(const std::string_view word) {
    std::size_t current_id = k_root_id;
    for (const char ch : word) {
      current_id = find_or_insert_child(current_id, ch);
    }
    nodes_[current_id].is_terminal = true;
  }

  // Returns all words stored in the trie that begin with the given prefix.
  // The prefix itself is included if it was directly inserted.
  [[nodiscard]] auto completions(const std::string_view prefix) const -> std::vector<std::string> {
    std::size_t current_id = k_root_id;
    for (const char ch : prefix) {
      const auto child_id = find_child(current_id, ch);
      if (!child_id.has_value()) { return {}; }
      current_id = *child_id;
    }

    std::vector<std::string> results;
    std::string accumulator{prefix};
    collect_words(current_id, accumulator, results);
    return results;
  }

  // Returns true if no symbols have been loaded.
  [[nodiscard]] auto empty() const -> bool {
    return nodes_.size() == 1U && nodes_[k_root_id].children.empty() && !nodes_[k_root_id].is_terminal;
  }

  // Removes all inserted symbols.
  void clear() {
    nodes_.clear();
    nodes_.emplace_back();
  }

private:
  struct Node {
    struct Child {
      char ch{0};
      std::size_t node_id{0};
    };

    std::vector<Child> children{};
    bool is_terminal{false};
  };

  static constexpr std::size_t k_root_id = 0U;

  [[nodiscard]] static auto lower_bound_child(Node& node, const char ch) -> std::vector<Node::Child>::iterator {
    return std::lower_bound(node.children.begin(), node.children.end(), ch,
                            [](const Node::Child& child, const char value) { return child.ch < value; });
  }

  [[nodiscard]] static auto lower_bound_child(const Node& node, const char ch) -> std::vector<Node::Child>::const_iterator {
    return std::lower_bound(node.children.begin(), node.children.end(), ch,
                            [](const Node::Child& child, const char value) { return child.ch < value; });
  }

  [[nodiscard]] auto append_node() -> std::size_t {
    nodes_.emplace_back();
    return nodes_.size() - 1U;
  }

  [[nodiscard]] auto find_or_insert_child(const std::size_t node_id, const char ch) -> std::size_t {
    {
      auto& node = nodes_[node_id];
      if (const auto it = lower_bound_child(node, ch); it != node.children.end() && it->ch == ch) {
        return it->node_id;
      }
    }

    const auto child_id = append_node();
    auto& node = nodes_[node_id];
    const auto it = lower_bound_child(node, ch);
    node.children.insert(it, Node::Child{.ch = ch, .node_id = child_id});
    return child_id;
  }

  [[nodiscard]] auto find_child(const std::size_t node_id, const char ch) const -> std::optional<std::size_t> {
    const auto& node = nodes_[node_id];
    const auto it = lower_bound_child(node, ch);
    if (it == node.children.end() || it->ch != ch) {
      return std::nullopt;
    }
    return it->node_id;
  }

  void collect_words(const std::size_t node_id, std::string& current, std::vector<std::string>& out) const {
    const auto& [children, is_terminal] = nodes_[node_id];
    if (is_terminal) { out.push_back(current); }
    for (const auto& [ch, child_node_id] : children) {
      current.push_back(ch);
      collect_words(child_node_id, current, out);
      current.pop_back();
    }
  }

  std::vector<Node> nodes_{1};
};

template <typename T>
class TrieMap {
public:
  void insert_or_assign(const std::string_view key, T value) {
    std::size_t current_id = k_root_id;
    for (const char ch : key) {
      current_id = find_or_insert_child(current_id, ch);
    }
    nodes_[current_id].value = std::move(value);
  }

  [[nodiscard]] auto find(const std::string_view key) -> std::optional<std::reference_wrapper<T>> {
    const auto node_id = find_node(key);
    if (!node_id.has_value()) {
      return std::nullopt;
    }
    auto& node = nodes_[*node_id];
    if (!node.value.has_value()) {
      return std::nullopt;
    }
    return std::ref(*node.value);
  }

  [[nodiscard]] auto find(const std::string_view key) const -> std::optional<std::reference_wrapper<const T>> {
    const auto node_id = find_node(key);
    if (!node_id.has_value()) {
      return std::nullopt;
    }
    const auto& node = nodes_[*node_id];
    if (!node.value.has_value()) {
      return std::nullopt;
    }
    return std::cref(*node.value);
  }

  [[nodiscard]] auto completions(const std::string_view prefix) const -> std::vector<std::string> {
    const auto node_id = find_node(prefix);
    if (!node_id.has_value()) {
      return {};
    }

    std::vector<std::string> results;
    std::string accumulator{prefix};
    collect_keys(*node_id, accumulator, results);
    return results;
  }

  [[nodiscard]] auto empty() const -> bool {
    return nodes_.size() == 1U && nodes_[k_root_id].children.empty() && !nodes_[k_root_id].value.has_value();
  }

  void clear() {
    nodes_.clear();
    nodes_.emplace_back();
  }

private:
  struct Node {
    struct Child {
      char ch{0};
      std::size_t node_id{0};
    };

    std::vector<Child> children{};
    std::optional<T> value{};
  };

  static constexpr std::size_t k_root_id = 0U;

  [[nodiscard]] static auto lower_bound_child(Node& node, const char ch) {
    return std::lower_bound(node.children.begin(), node.children.end(), ch,
                            [](const typename Node::Child& child, const char value) { return child.ch < value; });
  }

  [[nodiscard]] static auto lower_bound_child(const Node& node, const char ch) {
    return std::lower_bound(node.children.begin(), node.children.end(), ch,
                            [](const typename Node::Child& child, const char value) { return child.ch < value; });
  }

  [[nodiscard]] auto append_node() -> std::size_t {
    nodes_.emplace_back();
    return nodes_.size() - 1U;
  }

  [[nodiscard]] auto find_or_insert_child(const std::size_t node_id, const char ch) -> std::size_t {
    {
      auto& node = nodes_[node_id];
      if (const auto it = lower_bound_child(node, ch); it != node.children.end() && it->ch == ch) {
        return it->node_id;
      }
    }

    const auto child_id = append_node();
    auto& node = nodes_[node_id];
    const auto it = lower_bound_child(node, ch);
    node.children.insert(it, typename Node::Child{ch, child_id});
    return child_id;
  }

  [[nodiscard]] auto find_child(const std::size_t node_id, const char ch) const -> std::optional<std::size_t> {
    const auto& node = nodes_[node_id];
    const auto it = lower_bound_child(node, ch);
    if (it == node.children.end() || it->ch != ch) {
      return std::nullopt;
    }
    return it->node_id;
  }

  [[nodiscard]] auto find_node(const std::string_view key) const -> std::optional<std::size_t> {
    std::size_t current_id = k_root_id;
    for (const char ch : key) {
      const auto child_id = find_child(current_id, ch);
      if (!child_id.has_value()) {
        return std::nullopt;
      }
      current_id = *child_id;
    }
    return current_id;
  }

  void collect_keys(const std::size_t node_id, std::string& current, std::vector<std::string>& out) const {
    const auto& node = nodes_[node_id];
    if (node.value.has_value()) {
      out.push_back(current);
    }
    for (const auto& [ch, child_node_id] : node.children) {
      current.push_back(ch);
      collect_keys(child_node_id, current, out);
      current.pop_back();
    }
  }

  std::vector<Node> nodes_{1};
};

}  // namespace fleaux::common
