/*************************************************************************
 *
 * Copyright 2019 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef REALM_DICTIONARY_HPP
#define REALM_DICTIONARY_HPP

#include <realm/collection.hpp>
#include <realm/obj.hpp>
#include <realm/mixed.hpp>
#include <realm/column_mixed.hpp>

namespace realm {

class DictionaryBase : public CollectionBase {
public:
    using CollectionBase::CollectionBase;

protected:
    static constexpr CollectionType s_collection_type = CollectionType::Dictionary;
};

class Dictionary final : public CollectionBaseImpl<DictionaryBase>, public CollectionParent {
public:
    using Base = CollectionBaseImpl<DictionaryBase>;
    class Iterator;

    Dictionary() = default;
    ~Dictionary();

    Dictionary(const Obj& obj, ColKey col_key)
        : Dictionary(col_key, 1)
    {
        this->set_owner(obj, col_key);
    }
    Dictionary(CollectionParent& parent, Index index)
        : Base(parent, index)
    {
    }
    Dictionary(ColKey col_key, size_t level = 1);
    Dictionary(const Dictionary& other)
        : Base(static_cast<const Base&>(other))
        , CollectionParent(other.get_level())
        , m_key_type(other.m_key_type)
    {
        *this = other;
    }
    Dictionary& operator=(const Dictionary& other);

    DataType get_key_data_type() const;
    DataType get_value_data_type() const;

    std::pair<Mixed, Mixed> get_pair(size_t ndx) const;
    Mixed get_key(size_t ndx) const;
    PathElement get_path_element(size_t ndx) const override
    {
        return {get_key(ndx).get_string()};
    }

    // Overriding members of CollectionBase:
    CollectionBasePtr clone_collection() const final;
    size_t size() const final;
    bool is_null(size_t ndx) const final;
    Mixed get_any(size_t ndx) const final;
    size_t find_any(Mixed value) const final;
    size_t find_any_key(Mixed value) const noexcept;

    util::Optional<Mixed> min(size_t* return_ndx = nullptr) const final;
    util::Optional<Mixed> max(size_t* return_ndx = nullptr) const final;
    util::Optional<Mixed> sum(size_t* return_cnt = nullptr) const final;
    util::Optional<Mixed> avg(size_t* return_cnt = nullptr) const final;

    void sort(std::vector<size_t>& indices, bool ascending = true) const final;
    void distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const final;
    void sort_keys(std::vector<size_t>& indices, bool ascending = true) const;
    void distinct_keys(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const;

    void set_owner(const Obj& obj, ColKey ck) override
    {
        Base::set_owner(obj, ck);
        get_key_type();
    }
    void set_owner(std::shared_ptr<CollectionParent> parent, CollectionParent::Index index) override
    {
        Base::set_owner(std::move(parent), index);
        get_key_type();
    }

    // first points to inserted/updated element.
    // second is true if the element was inserted
    std::pair<Iterator, bool> insert(Mixed key, Mixed value);
    std::pair<Iterator, bool> insert(Mixed key, const Obj& obj);

    template <typename T>
    void insert_json(const std::string&, const T&);

    Obj create_and_insert_linked_object(Mixed key);

    void insert_collection(const PathElement&, CollectionType dict_or_list) override;
    DictionaryPtr get_dictionary(const PathElement& path_elem) const override;
    ListMixedPtr get_list(const PathElement& path_elem) const override;

    // throws std::out_of_range if key is not found
    Mixed get(Mixed key) const;
    // Noexcept version
    util::Optional<Mixed> try_get(Mixed key) const;
    // adds entry if key is not found
    const Mixed operator[](Mixed key);

    Obj get_object(StringData key);

    bool contains(Mixed key) const noexcept;
    Iterator find(Mixed key) const noexcept;

    void erase(Mixed key);
    Iterator erase(Iterator it);
    bool try_erase(Mixed key);

    void nullify(size_t);
    bool nullify(ObjLink target_link);
    bool replace_link(ObjLink old_link, ObjLink replace_link);
    bool remove_backlinks(CascadeState& state) const;
    size_t find_first(Mixed value) const;

    void clear() final;

    template <class T>
    void for_all_values(T&& f)
    {
        if (update()) {
            BPlusTree<Mixed> values(get_alloc());
            values.init_from_ref(m_dictionary_top->get_as_ref(1));
            auto func = [&f](BPlusTreeNode* node, size_t) {
                auto leaf = static_cast<BPlusTree<Mixed>::LeafNode*>(node);
                size_t sz = leaf->size();
                for (size_t i = 0; i < sz; i++) {
                    f(leaf->get(i));
                }
                return IteratorControl::AdvanceToNext;
            };

            values.traverse(func);
        }
    }

    template <class T, class Func>
    void for_all_keys(Func&& f)
    {
        if (update()) {
            BPlusTree<T> keys(get_alloc());
            keys.init_from_ref(m_dictionary_top->get_as_ref(0));
            auto func = [&f](BPlusTreeNode* node, size_t) {
                auto leaf = static_cast<typename BPlusTree<T>::LeafNode*>(node);
                size_t sz = leaf->size();
                for (size_t i = 0; i < sz; i++) {
                    f(leaf->get(i));
                }
                return IteratorControl::AdvanceToNext;
            };

            keys.traverse(func);
        }
    }


    Iterator begin() const;
    Iterator end() const;

    void migrate();

    // Overriding members in CollectionParent
    FullPath get_path() const override
    {
        return Base::get_path();
    }

    Path get_short_path() const override
    {
        return Base::get_short_path();
    }

    ColKey get_col_key() const noexcept override
    {
        return Base::get_col_key();
    }

    StablePath get_stable_path() const override
    {
        return Base::get_stable_path();
    }

    void add_index(Path& path, const Index& ndx) const final;
    size_t find_index(const Index&) const final;

    TableRef get_table() const noexcept override
    {
        return get_obj().get_table();
    }
    UpdateStatus update_if_needed() const override;
    const Obj& get_object() const noexcept override
    {
        return get_obj();
    }
    uint32_t parent_version() const noexcept override
    {
        return m_parent_version;
    }
    ref_type get_collection_ref(Index, CollectionType) const override;
    bool check_collection_ref(Index, CollectionType) const noexcept override;
    void set_collection_ref(Index, ref_type ref, CollectionType) override;
    StableIndex build_index(Mixed key) const;

    void to_json(std::ostream&, JSONOutputMode, util::FunctionRef<void(const Mixed&)>) const override;

    LinkCollectionPtr clone_as_obj_list() const final;

private:
    using Base::set_collection;

    template <typename T, typename Op>
    friend class CollectionColumnAggregate;
    friend class DictionaryLinkValues;
    friend class Cluster;

    mutable std::unique_ptr<Array> m_dictionary_top;
    mutable std::unique_ptr<BPlusTreeBase> m_keys;
    mutable std::unique_ptr<BPlusTreeMixed> m_values;
    DataType m_key_type = type_String;

    Dictionary(Allocator& alloc, ColKey col_key, ref_type ref);

    UpdateStatus init_from_parent(bool allow_create) const;
    Mixed do_get(size_t ndx) const;
    void do_erase(size_t ndx, Mixed key);
    Mixed do_get_key(size_t ndx) const;
    size_t do_find_key(Mixed key) const noexcept;
    std::pair<size_t, Mixed> find_impl(Mixed key) const noexcept;
    std::pair<Mixed, Mixed> do_get_pair(size_t ndx) const;
    bool clear_backlink(size_t ndx, CascadeState& state) const;
    void align_indices(std::vector<size_t>& indices) const;
    void swap_content(Array& fields1, Array& fields2, size_t index1, size_t index2);

    util::Optional<Mixed> do_min(size_t* return_ndx = nullptr) const;
    util::Optional<Mixed> do_max(size_t* return_ndx = nullptr) const;
    util::Optional<Mixed> do_sum(size_t* return_cnt = nullptr) const;
    util::Optional<Mixed> do_avg(size_t* return_cnt = nullptr) const;

    Mixed find_value(Mixed) const noexcept;

    template <typename AggregateType>
    void do_accumulate(size_t* return_ndx, AggregateType& agg) const;

    void ensure_created();
    bool update() const
    {
        return update_if_needed() != UpdateStatus::Detached;
    }
    void verify() const;
    void get_key_type();

    UpdateStatus do_update_if_needed(bool allow_create) const;
};

class Dictionary::Iterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = std::pair<Mixed, Mixed>;
    using difference_type = ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    pointer operator->() const
    {
        m_val = m_list->get_pair(m_ndx);
        return &m_val;
    }

    reference operator*() const
    {
        return *operator->();
    }

    Iterator& operator++() noexcept
    {
        ++m_ndx;
        return *this;
    }

    Iterator operator++(int) noexcept
    {
        auto tmp = *this;
        operator++();
        return tmp;
    }

    Iterator& operator--() noexcept
    {
        --m_ndx;
        return *this;
    }

    Iterator operator--(int) noexcept
    {
        auto tmp = *this;
        operator--();
        return tmp;
    }

    Iterator& operator+=(ptrdiff_t n) noexcept
    {
        m_ndx += n;
        return *this;
    }

    Iterator& operator-=(ptrdiff_t n) noexcept
    {
        m_ndx -= n;
        return *this;
    }

    friend ptrdiff_t operator-(const Iterator& lhs, const Iterator& rhs) noexcept
    {
        return ptrdiff_t(lhs.m_ndx) - ptrdiff_t(rhs.m_ndx);
    }

    friend Iterator operator+(Iterator lhs, ptrdiff_t rhs) noexcept
    {
        lhs.m_ndx += rhs;
        return lhs;
    }

    friend Iterator operator+(ptrdiff_t lhs, Iterator rhs) noexcept
    {
        return rhs + lhs;
    }

    bool operator!=(const Iterator& rhs) const noexcept
    {
        REALM_ASSERT_DEBUG(m_list == rhs.m_list);
        return m_ndx != rhs.m_ndx;
    }

    bool operator==(const Iterator& rhs) const noexcept
    {
        REALM_ASSERT_DEBUG(m_list == rhs.m_list);
        return m_ndx == rhs.m_ndx;
    }

    size_t index() const noexcept
    {
        return m_ndx;
    }

private:
    Iterator(const Dictionary* l, size_t ndx) noexcept
        : m_list(l)
        , m_ndx(ndx)
    {
    }

    friend class Dictionary;
    mutable value_type m_val;
    const Dictionary* m_list;
    size_t m_ndx = size_t(-1);
};

// An interface used when the value type of the dictionary consists of
// links to a single table. Implementation of the ObjList interface on
// top of a Dictionary of objects. This is the dictionary equivilent of
// LnkLst and LnkSet.
class DictionaryLinkValues final : public ObjCollectionBase<CollectionBase> {
public:
    DictionaryLinkValues() = default;
    DictionaryLinkValues(const Obj& obj, ColKey col_key);
    DictionaryLinkValues(const Dictionary& source);

    // Overrides of ObjList:
    ObjKey get_key(size_t ndx) const final;
    Obj get_object(size_t row_ndx) const final;

    // Overrides of CollectionBase, these simply forward to the underlying dictionary.
    size_t size() const final
    {
        return m_source.size();
    }
    bool is_null(size_t ndx) const final
    {
        return m_source.is_null(ndx);
    }
    Mixed get_any(size_t ndx) const final
    {
        return m_source.get_any(ndx);
    }
    void clear() final
    {
        m_source.clear();
    }
    util::Optional<Mixed> min(size_t* return_ndx = nullptr) const final
    {
        return m_source.min(return_ndx);
    }
    util::Optional<Mixed> max(size_t* return_ndx = nullptr) const final
    {
        return m_source.max(return_ndx);
    }
    util::Optional<Mixed> sum(size_t* return_cnt = nullptr) const final
    {
        return m_source.sum(return_cnt);
    }
    util::Optional<Mixed> avg(size_t* return_cnt = nullptr) const final
    {
        return m_source.avg(return_cnt);
    }
    CollectionBasePtr clone_collection() const final
    {
        return std::make_unique<DictionaryLinkValues>(m_source);
    }
    LinkCollectionPtr clone_obj_list() const final
    {
        return std::make_unique<DictionaryLinkValues>(m_source);
    }
    void sort(std::vector<size_t>& indices, bool ascending = true) const final
    {
        m_source.sort(indices, ascending);
    }
    void distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const final
    {
        m_source.distinct(indices, sort_order);
    }
    size_t find_any(Mixed value) const final
    {
        return m_source.find_any(value);
    }
    const Obj& get_obj() const noexcept final
    {
        return m_source.get_obj();
    }
    ColKey get_col_key() const noexcept final
    {
        return m_source.get_col_key();
    }
    bool has_changed() const noexcept final
    {
        return m_source.has_changed();
    }
    CollectionType get_collection_type() const noexcept override
    {
        return CollectionType::List;
    }

    // Overrides of ObjCollectionBase:
    UpdateStatus do_update_if_needed() const final
    {
        return m_source.update_if_needed();
    }
    BPlusTree<ObjKey>* get_mutable_tree() const final
    {
        // We are faking being an ObjList because the underlying storage is not
        // actually a BPlusTree<ObjKey> for dictionaries it is all mixed values.
        // But this is ok, because we don't need to deal with unresolved link
        // maintenance because they are not hidden from view in dictionaries in
        // the same way as for LnkSet and LnkLst. This means that the functions
        // that call get_mutable_tree do not need to do anything for dictionaries.
        return nullptr;
    }

    void set_owner(const Obj& obj, ColKey ck) override
    {
        m_source.set_owner(obj, ck);
    }

    void set_owner(std::shared_ptr<CollectionParent> parent, CollectionParent::Index index) override
    {
        m_source.set_owner(std::move(parent), index);
    }

    FullPath get_path() const noexcept final
    {
        return m_source.get_path();
    }

    Path get_short_path() const noexcept final
    {
        return m_source.get_short_path();
    }

    StablePath get_stable_path() const noexcept final
    {
        return m_source.get_stable_path();
    }

private:
    Dictionary m_source;
};


inline std::pair<Dictionary::Iterator, bool> Dictionary::insert(Mixed key, const Obj& obj)
{
    return insert(key, Mixed(obj.get_link()));
}

inline CollectionBasePtr Dictionary::clone_collection() const
{
    return std::make_unique<Dictionary>(m_obj_mem, this->get_col_key());
}


} // namespace realm

#endif /* SRC_REALM_DICTIONARY_HPP_ */
