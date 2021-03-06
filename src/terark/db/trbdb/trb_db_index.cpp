#include "trb_db_index.hpp"
#include "trb_db_context.hpp"
#include <terark/util/fstrvec.hpp>
#include <terark/io/FileStream.hpp>
#include <terark/io/StreamBuffer.hpp>
#include <terark/io/DataIO.hpp>
#include <terark/io/var_int.hpp>
#include <type_traits>
#include <tbb/mutex.h>
#include <terark/threaded_rb_tree.h>
#include <terark/mempool.hpp>
#include <terark/lcast.hpp>
#include <boost/filesystem.hpp>


namespace fs = boost::filesystem;
using namespace terark;
using namespace terark::db;

namespace terark { namespace db { namespace trbdb {

template<class Key, class Fixed>
class TrbIndexIterForward;
template<class Key, class Fixed>
class TrbIndexIterBackward;

template<class Key, class Fixed>
class TrbIndexStoreIterForward;
template<class Key, class Fixed>
class TrbIndexStoreIterBackward;


// var length
// fixed length
// fixed length aligned
// numeric

template<class Key, class Fixed>
class TrbWritableIndexTemplate : public TrbWritableIndex
{
    friend class TrbIndexIterForward<Key, Fixed>;
    friend class TrbIndexIterBackward<Key, Fixed>;
    friend class TrbIndexStoreIterForward<Key, Fixed>;
    friend class TrbIndexStoreIterBackward<Key, Fixed>;

    typedef size_t size_type;
    typedef threaded_rb_tree_node_t<uint32_t> node_type;
    typedef threaded_rb_tree_root_t<node_type, std::true_type, std::true_type> root_type;
    static size_type constexpr max_stack_depth = 2 * (sizeof(uint32_t) * 8 - 1);

    template<class Storage>
    struct mutable_deref_node_t
    {
        node_type &operator()(size_type index) const
        {
            return s.node(index);
        }
        Storage &s;
    };
    template<class Storage>
    static mutable_deref_node_t<Storage> mutable_deref_node(Storage &s)
    {
        return mutable_deref_node_t<Storage>{s};
    }
    template<class Storage>
    struct const_deref_node_t
    {
        node_type const &operator()(size_type index) const
        {
            return s.node(index);
        }
        Storage const &s;
    };
    template<class Storage>
    static const_deref_node_t<Storage> const_deref_node(Storage const &s)
    {
        return const_deref_node_t<Storage>{s};
    }
    template<class Storage>
    struct deref_key_t
    {
        fstring operator()(size_type index) const
        {
            return s.key(index);
        }
        Storage const &s;
    };
    template<class Storage>
    static deref_key_t<Storage> deref_key(Storage const &s)
    {
        return deref_key_t<Storage>{s};
    }

    template<class Storage>
    struct normal_key_compare_type
    {
        bool operator()(fstring left, fstring right) const
        {
            return left < right;
        }
        bool operator()(size_type left, size_type right) const
        {
            int c = fstring_func::compare3()(storage.key(left), storage.key(right));
            if(c == 0)
            {
                return left > right;
            }
            return c < 0;
        }
        int compare(fstring left, fstring right) const
        {
            return fstring_func::compare3()(left, right);
        }
        int compare(size_type left, size_type right) const
        {
            int c = fstring_func::compare3()(storage.key(left), storage.key(right));
            if(c == 0)
            {
                if(left == right)
                {
                    return 0;
                }
                return left > right ? -1 : 1;
            }
            return c;
        }
        Storage const &storage;
    };
    template<class Storage>
    struct numeric_key_compare_type
    {
        bool operator()(fstring left, fstring right) const
        {
            assert(reinterpret_cast<size_type>(left.data()) % sizeof(Key) == 0);
            assert(reinterpret_cast<size_type>(right.data()) % sizeof(Key) == 0);

            assert(left.size() == sizeof(Key));
            assert(right.size() == sizeof(Key));

            auto left_key = *reinterpret_cast<Key const *>(left.data());
            auto right_key = *reinterpret_cast<Key const *>(right.data());

            return left_key < right_key;
        }
        bool operator()(size_type left, size_type right) const
        {
            assert(reinterpret_cast<size_type>(storage.key_ptr(left)) % sizeof(Key) == 0);
            assert(reinterpret_cast<size_type>(storage.key_ptr(right)) % sizeof(Key) == 0);

            assert(storage.key_len(left) == sizeof(Key));
            assert(storage.key_len(right) == sizeof(Key));

            auto left_key = *reinterpret_cast<Key const *>(storage.key_ptr(left));
            auto right_key = *reinterpret_cast<Key const *>(storage.key_ptr(right));

            if(left_key < right_key)
            {
                return true;
            }
            else if(right_key < left_key)
            {
                return false;
            }
            else
            {
                return left > right;
            }
        }
        Storage const &storage;
    };

    struct normal_storage_type
    {
        typedef terark::MemPool<4> pool_type;
        struct element_type
        {
            node_type node;
            uint32_t offset;
        };
        struct data_object
        {
            byte data[1];
        };
        root_type root;
        valvec<element_type> index;
        pool_type data;
        size_type total;

        normal_storage_type(size_type fixed_length) : data(256), total()
        {
            assert(fixed_length == 0);
        }

        size_type key_count() const
        {
            return root.get_count();
        }
        size_type total_length() const
        {
            return total;
        }
        size_type max_index() const
        {
            return index.size();
        }
        size_type memory_size() const
        {
            return sizeof(*this) + data.size() + index.size() * sizeof(element_type);
        }

        node_type &node(size_type i)
        {
            return index[i].node;
        }
        node_type const &node(size_type i) const
        {
            return index[i].node;
        }

        fstring key(size_type i) const
        {
            byte const *ptr;
            size_type len = load_var_uint32(data.at<data_object>(index[i].offset).data, &ptr);
            return fstring(ptr, len);
        }
        byte const *key_ptr(size_type i) const
        {
            byte const *ptr;
            load_var_uint32(data.at<data_object>(index[i].offset).data, &ptr);
            return ptr;
        }
        size_type key_len(size_type i) const
        {
            byte const *ptr;
            return load_var_uint32(data.at<data_object>(index[i].offset).data, &ptr);
        }

        template<class Compare>
        bool store_check(size_type i, fstring d)
        {
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_unique(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                d,
                                                                deref_key(*this),
                                                                Compare{*this}
            );
            if(exists)
            {
                return stack.get_index(stack.height - 1) == i;
            }
            if(terark_likely(i >= index.size()))
            {
                index.resize(i + 1, element_type{{0xFFFFFFFFU, 0xFFFFFFFFU}, std::numeric_limits<uint32_t>::max()});
            }
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
                threaded_rb_tree_find_path_for_unique(root,
                                                      stack,
                                                      const_deref_node(*this),
                                                      d,
                                                      deref_key(*this),
                                                      Compare{*this}
                );
            }
            assert(node(i).is_empty());
            byte len_data[8];
            byte *end_ptr = save_var_uint32(len_data, uint32_t(d.size()));
            size_type len_len = size_type(end_ptr - len_data);
            size_type dst_len = pool_type::align_to(d.size() + len_len);
            index[i].offset = data.alloc(dst_len);
            byte *dst_ptr = data.at<data_object>(index[i].offset).data;
            std::memcpy(dst_ptr, len_data, len_len);
            std::memcpy(dst_ptr + len_len, d.data(), d.size());
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
            total += d.size();
            return true;
        }
        template<class Compare>
        void store_cover(size_type i, fstring d)
        {
            if(terark_likely(i >= index.size()))
            {
                index.resize(i + 1, element_type{{0xFFFFFFFFU, 0xFFFFFFFFU}, 0xFFFFFFFFU});
            }
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
            }
            byte len_data[8];
            byte *end_ptr = save_var_uint32(len_data, uint32_t(d.size()));
            size_type len_len = size_type(end_ptr - len_data);
            size_type dst_len = pool_type::align_to(d.size() + len_len);
            index[i].offset = data.alloc(dst_len);
            byte *dst_ptr = data.at<data_object>(index[i].offset).data;
            std::memcpy(dst_ptr, len_data, len_len);
            std::memcpy(dst_ptr + len_len, d.data(), d.size());
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            threaded_rb_tree_find_path_for_multi(root,
                                                 stack,
                                                 const_deref_node(*this),
                                                 i,
                                                 Compare{*this}
            );
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
            size_type c;
            if(false
               || (
                   i != root.get_most_left(const_deref_node(*this))
                   &&
                   !Compare{*this}(c = threaded_rb_tree_move_prev(i, const_deref_node(*this)), i)
                   )
               || (
                   i != root.get_most_right(const_deref_node(*this))
                   &&
                   !Compare{*this}(i, c = threaded_rb_tree_move_next(i, const_deref_node(*this)))
                   )
               )
            {
                data.sfree(index[i].offset, dst_len);
                index[i].offset = index[c].offset;
            }
            total += d.size();
        }
        template<class Compare>
        void remove(size_type i)
        {
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_remove(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                i,
                                                                Compare{*this}
            );
            assert(exists);
            (void)exists;
            byte const *ptr = data.at<data_object>(index[i].offset).data, *end_ptr;
            size_type len = load_var_uint32(ptr, &end_ptr);
            if(true
               && (
                   i == root.get_most_left(const_deref_node(*this))
                   ||
                   Compare{*this}(threaded_rb_tree_move_prev(i, const_deref_node(*this)), i)
                   )
               && (
                   i == root.get_most_right(const_deref_node(*this))
                   ||
                   Compare{*this}(i, threaded_rb_tree_move_next(i, const_deref_node(*this)))
                   )
               )
            {
                data.sfree(index[i].offset, pool_type::align_to(end_ptr - ptr + len));
            }
            threaded_rb_tree_remove(root,
                                    stack,
                                    mutable_deref_node(*this)
            );
            total -= len;
        }

        void clear()
        {
            root = root_type();
            index.clear();
            data.clear();
        }
        void shrink_to_fit()
        {
            index.shrink_to_fit();
            data.shrink_to_fit();
        }
    };
    struct fixed_storage_type
    {
        root_type root;
        valvec<node_type> index;
        valvec<byte> data;
        size_type key_length;

        fixed_storage_type(size_type fixed_length) : key_length(fixed_length)
        {
            assert(fixed_length > 0);
        }

        size_type key_count() const
        {
            return root.get_count();
        }
        size_type total_length() const
        {
            return root.get_count() * key_length;
        }
        size_type max_index() const
        {
            return index.size();
        }
        size_type memory_size() const
        {
            return sizeof(*this) + data.size() + index.size() * sizeof(node_type);
        }

        node_type &node(size_type i)
        {
            return index[i];
        }
        node_type const &node(size_type i) const
        {
            return index[i];
        }

        fstring key(size_type i) const
        {
            return fstring(data.data() + i * key_length, key_length);
        }
        byte const *key_ptr(size_type i) const
        {
            return data.data() + i * key_length;
        }
        size_type key_len(size_type i) const
        {
            return key_length;
        }

        template<class Compare>
        bool store_check(size_type i, fstring d)
        {
            assert(d.size() == key_length);
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_unique(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                d,
                                                                deref_key(*this),
                                                                Compare{*this}
            );
            if(exists)
            {
                return stack.get_index(stack.height - 1) == i;
            }
            if(terark_likely(i >= index.size()))
            {
                index.resize(i + 1, node_type{0xFFFFFFFFU, 0xFFFFFFFFU});
                data.resize_no_init(index.size() * key_length);
            }
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
                std::memcpy(data.data() + i * key_length, d.data(), d.size());
                threaded_rb_tree_find_path_for_multi(root,
                                                     stack,
                                                     const_deref_node(*this),
                                                     i,
                                                     Compare{*this}
                );
            }
            else
            {
                std::memcpy(data.data() + i * key_length, d.data(), d.size());
            }
            assert(node(i).is_empty());
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
            return true;
        }
        template<class Compare>
        void store_cover(size_type i, fstring d)
        {
            assert(d.size() == key_length);
            if(terark_likely(i >= index.size()))
            {
                index.resize(i + 1, node_type{0xFFFFFFFFU, 0xFFFFFFFFU});
                data.resize_no_init(index.size() * key_length);
            }
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
            }
            std::memcpy(data.data() + i * key_length, d.data(), d.size());
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            threaded_rb_tree_find_path_for_multi(root,
                                                 stack,
                                                 const_deref_node(*this),
                                                 i,
                                                 Compare{*this}
            );
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
        }
        template<class Compare>
        void remove(size_type i)
        {
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_remove(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                i,
                                                                Compare{*this}
            );
            assert(exists);
            (void)exists;
            threaded_rb_tree_remove(root,
                                    stack,
                                    mutable_deref_node(*this)
            );
        }

        void clear()
        {
            root = root_type();
            index.clear();
            data.clear();
        }
        void shrink_to_fit()
        {
            index.shrink_to_fit();
            data.shrink_to_fit();
        }
    };
    struct aligned_fixed_storage_type
    {
        struct element_type
        {
            node_type node;
            byte data[4];
        };
        root_type root;
        valvec<byte> index;
        size_type element_length;

        aligned_fixed_storage_type(size_type fixed_length) : element_length(fixed_length + sizeof(node_type))
        {
            assert(fixed_length > 0 && fixed_length % 4 == 0);
        }

        size_type key_count() const
        {
            return root.get_count();
        }
        size_type total_length() const
        {
            return root.get_count() * (element_length - sizeof(node_type));
        }
        size_type max_index() const
        {
            return index.size() / element_length;
        }
        size_type memory_size() const
        {
            return sizeof(*this) + index.size();
        }

        node_type &node(size_type i)
        {
            element_type *ptr = reinterpret_cast<element_type *>(index.data() + i * element_length);
            return ptr->node;
        }
        node_type const &node(size_type i) const
        {
            element_type const *ptr = reinterpret_cast<element_type const *>(index.data() + i * element_length);
            return ptr->node;
        }

        fstring key(size_type i) const
        {
            element_type const *ptr = reinterpret_cast<element_type const *>(index.data() + i * element_length);
            return fstring(ptr->data, element_length - sizeof(node_type));
        }
        byte const *key_ptr(size_type i) const
        {
            return reinterpret_cast<element_type const *>(index.data() + i * element_length)->data;
        }
        size_type key_len(size_type i) const
        {
            return element_length - sizeof(node_type);
        }

        template<class Compare>
        bool store_check(size_type i, fstring d)
        {
            assert(d.size() + sizeof(node_type) == element_length);
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_unique(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                d,
                                                                deref_key(*this),
                                                                Compare{*this}
            );
            if(exists)
            {
                return stack.get_index(stack.height - 1) == i;
            }
            if(terark_likely(i * element_length >= index.size()))
            {
                index.resize((i + 1) * element_length, 0xFFU);
            }
            element_type *ptr = reinterpret_cast<element_type *>(index.data() + i * element_length);
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
                std::memcpy(ptr->data, d.data(), d.size());
                threaded_rb_tree_find_path_for_multi(root,
                                                     stack,
                                                     const_deref_node(*this),
                                                     i,
                                                     Compare{*this}
                );
            }
            else
            {
                std::memcpy(ptr->data, d.data(), d.size());
            }
            assert(node(i).is_empty());
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
            return true;
        }
        template<class Compare>
        void store_cover(size_type i, fstring d)
        {
            assert(d.size() + sizeof(node_type) == element_length);
            if(terark_likely(i * element_length >= index.size()))
            {
                index.resize((i + 1) * element_length, 0xFFU);
            }
            if(terark_unlikely(node(i).is_used()))
            {
                remove<Compare>(i);
            }
            assert(node(i).is_empty());
            element_type *ptr = reinterpret_cast<element_type *>(index.data() + i * element_length);
            std::memcpy(ptr->data, d.data(), d.size());
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            threaded_rb_tree_find_path_for_multi(root,
                                                 stack,
                                                 const_deref_node(*this),
                                                 i,
                                                 Compare{*this}
            );
            threaded_rb_tree_insert(root,
                                    stack,
                                    mutable_deref_node(*this),
                                    i
            );
        }
        template<class Compare>
        void remove(size_type i)
        {
            threaded_rb_tree_stack_t<node_type, max_stack_depth> stack;
            bool exists = threaded_rb_tree_find_path_for_remove(root,
                                                                stack,
                                                                const_deref_node(*this),
                                                                i,
                                                                Compare{*this}
            );
            assert(exists);
            (void)exists;
            threaded_rb_tree_remove(root,
                                    stack,
                                    mutable_deref_node(*this)
            );
        }

        void clear()
        {
            root = root_type();
            index.clear();
        }
        void shrink_to_fit()
        {
            index.shrink_to_fit();
        }
    };

    template<class T, class Unused>
    struct is_aligned : public std::conditional<
        sizeof(T) % sizeof(uint32_t) == 0
        , std::true_type
        , std::false_type
    >::type
    {
    };
    template<class Unused>
    struct is_aligned<void, Unused> : public std::false_type
    {
    };

    typedef typename std::conditional<Fixed::value
        || std::is_arithmetic<Key>::value
        , std::true_type
        , std::false_type
    >::type fixed_type;
    typedef typename std::conditional<!fixed_type::value
        , normal_storage_type,
        typename std::conditional<
        is_aligned<Key, void>::value
        , aligned_fixed_storage_type
        , fixed_storage_type
        >::type
    >::type storage_type;
    typedef typename std::conditional<
        std::is_arithmetic<Key>::value
        , numeric_key_compare_type<storage_type>
        , normal_key_compare_type<storage_type>
    >::type key_compare_type;

    storage_type m_storage;
    FileStream m_fp;
    NativeDataOutput<OutputBuffer> m_out;
    mutable SpinRwMutex m_rwMutex;

    static std::string fixFilePath(PathRef fpath)
    {
        return fstring(fpath.string()).endsWith(".trb")
            ? fpath.string()
            : fpath.string() + ".trb";
    }

public:
    explicit TrbWritableIndexTemplate(PathRef fpath, size_type fixedLen, bool isUnique)
        : m_storage(fixedLen)
        , m_fp(fixFilePath(fpath).c_str(), "wb")
    {
        ReadableIndex::m_isUnique = isUnique;

        m_fp.disbuf();
        NativeDataInput<InputBuffer> in; in.attach(&m_fp);
        uint32_t index_remove_replace, replace;
        std::string key;
        try
        {
            while(true)
            {
                // |   1  |   1   |  30 |
                // |remove|replace|index|
                // index_remove_replace
                //     if replace , read another index , move the key at other to index
                //     if remove , remove key at index
                //     otherwise , read key , insert or update key at index

                in >> index_remove_replace;
                if((index_remove_replace & 0xC0000000U) == 0)
                {
                    in >> key;
                    m_storage.template store_cover<key_compare_type>(index_remove_replace, key);
                }
                else if(index_remove_replace & 0x40000000U)
                {
                    in >> replace;
                    if((replace & 0xC0000000U) != 0)
                    {
                        //TODO WTF ? bad storage file ???
                    }
                    //TODO check replace exists !!!
                    fstring move_key = m_storage.key(replace);
                    m_storage.template store_cover<key_compare_type>(index_remove_replace & 0x3FFFFFFFU, move_key);
                    m_storage.template remove<key_compare_type>(replace);
                }
                else if(index_remove_replace & 0x80000000U)
                {
                    //TODO check index exists !!!
                    m_storage.template remove<key_compare_type>(index_remove_replace & 0x3FFFFFFFU);
                }
                else
                {
                    //TODO WTF ? bad storage file +10086 ???
                }
            }
        }
        catch(EndOfFileException const &e)
        {
            (void)e;//shut up !
        }
        m_out.attach(&m_fp);
    }
    void save(PathRef) const override
    {
        //nothing todo ...
    }
    void load(PathRef) override
    {
        //nothing todo ...
    }

    IndexIterator* createIndexIterForward(DbContext*) const override
    {
        return new TrbIndexIterForward<Key, Fixed>(this, ReadableIndex::m_isUnique);
    }
    IndexIterator* createIndexIterBackward(DbContext*) const override
    {
        return new TrbIndexIterBackward<Key, Fixed>(this, ReadableIndex::m_isUnique);
    }

    llong indexStorageSize() const override
    {
        return m_storage.memory_size();
    }

    bool remove(fstring key, llong id, DbContext*) override
    {
        assert(m_storage.key(id) == key);
        m_storage.template remove<key_compare_type>(id);
        m_out << (uint32_t(id) | 0x80000000U);
        m_out.flush();
        return true;
    }
    bool insert(fstring key, llong id, DbContext*) override
    {
        if(m_isUnique)
        {
            if(!m_storage.template store_check<key_compare_type>(id, key))
            {
                return false;
            }
        }
        else
        {
            m_storage.template store_cover<key_compare_type>(id, key);
        }
        m_out << uint32_t(id) << key;
        m_out.flush();
        return true;
    }
    bool replace(fstring key, llong oldId, llong newId, DbContext*) override
    {
        assert(key == m_storage.key(oldId));
        m_storage.template store_cover<key_compare_type>(newId, key);
        m_storage.template remove<key_compare_type>(oldId);
        m_out << uint32_t(newId) << uint32_t(oldId);
        m_out.flush();
        return true;
    }

    void clear() override
    {
        m_storage.clear();
    }

    void searchExactAppend(fstring key, valvec<llong>* recIdvec, DbContext*) const override
    {
        size_type lower, upper;
        threaded_rb_tree_equal_range(m_storage.root,
                                     const_deref_node(m_storage),
                                     key,
                                     deref_key(m_storage),
                                     key_compare_type{m_storage},
                                     lower,
                                     upper
        );
        while(lower != upper)
        {
            recIdvec->emplace_back(lower);
            lower = threaded_rb_tree_move_next(lower, const_deref_node(m_storage));
        }
    }


    llong dataStorageSize() const override
    {
        return m_storage.memory_size();
    }
    llong dataInflateSize() const override
    {
        return m_storage.total_length();
    }
    llong numDataRows() const override
    {
        return m_storage.max_index();
    }
    void getValueAppend(llong id, valvec<byte>* val, DbContext*) const override
    {
        fstring key = m_storage.key(size_t(id));
        val->append(key.begin(), key.end());
    }

    StoreIterator* createStoreIterForward(DbContext*) const override
    {
        return new TrbIndexStoreIterForward<Key, Fixed>(this);
    }
    StoreIterator* createStoreIterBackward(DbContext*) const override
    {
        return new TrbIndexStoreIterBackward<Key, Fixed>(this);
    }

    llong append(fstring row, DbContext*) override
    {
        size_t id = m_storage.max_index();
        if(m_isUnique)
        {
            bool success = m_storage.template store_check<key_compare_type>(id, row);
            assert(success);
            (void)success;
        }
        else
        {
            m_storage.template store_cover<key_compare_type>(id, row);
        }
        m_out << uint32_t(id) << row;
        m_out.flush();
        return llong(id);
    }
    void update(llong id, fstring row, DbContext*) override
    {
        if(m_isUnique)
        {
            bool success = m_storage.template store_check<key_compare_type>(id, row);
            assert(success);
            (void)success;
        }
        else
        {
            m_storage.template store_cover<key_compare_type>(id, row);
        }
        m_out << uint32_t(id) << row;
        m_out.flush();
    }
    void remove(llong id, DbContext*) override
    {
        m_storage.template remove<key_compare_type>(id);
        m_out << (uint32_t(id) | 0x80000000U);
        m_out.flush();
    }

    void shrinkToFit() override
    {
        m_storage.shrink_to_fit();
    }

    ReadableIndex* getReadableIndex() override
    {
        return this;
    }
    WritableIndex* getWritableIndex() override
    {
        return this;
    }
    ReadableStore* getReadableStore() override
    {
        return this;
    }
    AppendableStore* getAppendableStore() override
    {
        return this;
    }
    UpdatableStore* getUpdatableStore() override
    {
        return this;
    }
    WritableStore* getWritableStore() override
    {
        return this;
    }
};

template<class Key, class Fixed>
class TrbIndexIterForward : public IndexIterator
{
    typedef TrbWritableIndexTemplate<Key, Fixed> owner_t;
    boost::intrusive_ptr<owner_t> owner;
    size_t where;

public:
    TrbIndexIterForward(owner_t const *o, bool unique)
    {
        m_isUniqueInSchema = unique;
        owner.reset(const_cast<owner_t *>(o));
        where = o->m_storage.root.get_most_left(owner_t::const_deref_node(o->m_storage));
    }


    void reset() override
    {
        auto const *o = owner.get();
        where = o->m_storage.root.get_most_left(owner_t::const_deref_node(o->m_storage));
    }
    bool increment(llong* id, valvec<byte>* key) override
    {
        auto const *o = owner.get();
        if(terark_likely(where != owner_t::node_type::nil_sentinel))
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            key->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_next(where, owner_t::const_deref_node(o->m_storage));
            return true;
        }
        else
        {
            return false;
        }
    }

    int seekLowerBound(fstring key, llong* id, valvec<byte>* retKey) override
    {
        auto const *o = owner.get();
        where = threaded_rb_tree_lower_bound(o->m_storage.root,
                                             owner_t::const_deref_node(o->m_storage),
                                             key,
                                             owner_t::deref_key(o->m_storage),
                                             typename owner_t::key_compare_type{o->m_storage}
        );
        if(where != owner_t::node_type::nil_sentinel)
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            retKey->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_next(where, owner_t::const_deref_node(o->m_storage));
            if(typename owner_t::key_compare_type{o->m_storage}(storage_key, key))
            {
                return 1;
            }
            else
            {
                return 0;
            }
        }
        return -1;
    }
    int seekUpperBound(fstring key, llong* id, valvec<byte>* retKey) override
    {
        auto const *o = owner.get();
        where = threaded_rb_tree_upper_bound(o->m_storage.root,
                                             owner_t::const_deref_node(o->m_storage),
                                             key,
                                             owner_t::deref_key(o->m_storage),
                                             typename owner_t::key_compare_type{o->m_storage}
        );
        if(where != owner_t::node_type::nil_sentinel)
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            retKey->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_next(where, owner_t::const_deref_node(o->m_storage));
            return 1;
        }
        return -1;
    }
};

template<class Key, class Fixed>
class TrbIndexIterBackward : public IndexIterator
{
    typedef TrbWritableIndexTemplate<Key, Fixed> owner_t;
    boost::intrusive_ptr<owner_t> owner;
    size_t where;

public:
    TrbIndexIterBackward(owner_t const *o, bool unique)
    {
        m_isUniqueInSchema = unique;
        owner.reset(const_cast<owner_t *>(o));
        where = o->m_storage.root.get_most_left(owner_t::const_deref_node(o->m_storage));
    }


    void reset() override
    {
        auto const *o = owner.get();
        where = o->m_storage.root.get_most_left(owner_t::const_deref_node(o->m_storage));
    }
    bool increment(llong* id, valvec<byte>* key) override
    {
        auto const *o = owner.get();
        if(terark_likely(where != owner_t::node_type::nil_sentinel))
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            key->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_prev(where, owner_t::const_deref_node(o->m_storage));
            return true;
        }
        else
        {
            return false;
        }
    }

    int seekLowerBound(fstring key, llong* id, valvec<byte>* retKey) override
    {
        auto const *o = owner.get();
        where = threaded_rb_tree_reverse_lower_bound(o->m_storage.root,
                                                     owner_t::const_deref_node(o->m_storage),
                                                     key,
                                                     owner_t::deref_key(o->m_storage),
                                                     typename owner_t::key_compare_type{o->m_storage}
        );
        if(where != owner_t::node_type::nil_sentinel)
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            retKey->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_prev(where, owner_t::const_deref_node(o->m_storage));
            if(typename owner_t::key_compare_type{o->m_storage}(key, storage_key))
            {
                return 1;
            }
            else
            {
                return 0;
            }
        }
        return -1;
    }
    int seekUpperBound(fstring key, llong* id, valvec<byte>* retKey) override
    {
        auto const *o = owner.get();
        where = threaded_rb_tree_reverse_upper_bound(o->m_storage.root,
                                                     owner_t::const_deref_node(o->m_storage),
                                                     key,
                                                     owner_t::deref_key(o->m_storage),
                                                     typename owner_t::key_compare_type{o->m_storage}
        );
        if(where != owner_t::node_type::nil_sentinel)
        {
            auto storage_key = o->m_storage.key(where);
            *id = where;
            retKey->assign(storage_key.begin(), storage_key.end());
            where = threaded_rb_tree_move_prev(where, owner_t::const_deref_node(o->m_storage));
            return 1;
        }
        return -1;
    }
};

template<class Key, class Fixed>
class TrbIndexStoreIterForward : public StoreIterator
{
    typedef TrbWritableIndexTemplate<Key, Fixed> owner_t;
    size_t m_where;
public:
    TrbIndexStoreIterForward(owner_t const *o)
    {
        m_store.reset(const_cast<owner_t *>(o));
        m_where = 0;
    }
    bool increment(llong* id, valvec<byte>* val) override
    {
        auto const *o = static_cast<owner_t const *>(m_store.get());
        size_t max = o->m_storage.max_index();
        while(m_where < max)
        {
            size_t k = m_where++;
            if(o->m_storage.node(k).is_used())
            {
                fstring key = o->m_storage.key(k);
                *id = k;
                val->assign(key.begin(), key.end());
                return true;
            }
        }
        return false;
    }
    bool seekExact(llong id, valvec<byte>* val) override
    {
        auto const *o = static_cast<owner_t const *>(m_store.get());
        if(id < 0 || id >= llong(o->m_storage.max_index()))
        {
            THROW_STD(out_of_range, "Invalid id = %lld, rows = %zd"
                      , id, o->m_storage.max_index());
        }
        if(o->m_storage.node(id).is_used())
        {
            fstring key = o->m_storage.key(size_t(id));
            val->assign(key.begin(), key.end());
            return true;
        }
        return false;
    }
    void reset() override
    {
        m_where = 0;
    }
};

template<class Key, class Fixed>
class TrbIndexStoreIterBackward : public StoreIterator
{
    typedef TrbWritableIndexTemplate<Key, Fixed> owner_t;
    size_t m_where;
public:
    TrbIndexStoreIterBackward(owner_t const *o)
    {
        m_store.reset(const_cast<owner_t *>(o));
        m_where = o->m_storage.max_index();
    }
    bool increment(llong* id, valvec<byte>* val) override
    {
        auto const *o = static_cast<owner_t const *>(m_store.get());
        while(m_where > 0)
        {
            size_t k = --m_where;
            if(o->m_storage.node(k).is_used())
            {
                fstring key = o->m_storage.key(k);
                *id = k;
                val->assign(key.begin(), key.end());
                return true;
            }
        }
        return false;
    }
    bool seekExact(llong id, valvec<byte>* val) override
    {
        auto const *o = static_cast<owner_t const *>(m_store.get());
        if(id < 0 || id >= llong(o->m_storage.max_index()))
        {
            THROW_STD(out_of_range, "Invalid id = %lld, rows = %zd"
                      , id, o->m_storage.max_index());
        }
        if(o->m_storage.node(id).is_used())
        {
            fstring key = o->m_storage.key(size_t(id));
            val->assign(key.begin(), key.end());
            return true;
        }
        return false;
    }
    void reset() override
    {
        m_where = m_store->numDataRows();
    }
};

TrbWritableIndex *TrbWritableIndex::createIndex(Schema const &schema, PathRef fpath)
{
    if(schema.columnNum() == 1)
    {
        ColumnMeta cm = schema.getColumnMeta(0);
#define CASE_COL_TYPE(Enum, Type) \
    case ColumnType::Enum: return new TrbWritableIndexTemplate<Type, std::false_type>(fpath, schema.getFixedRowLen(), schema.m_isUnique);
        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        switch(cm.type)
        {
        default: break;
            CASE_COL_TYPE(Uint08, uint8_t);
            CASE_COL_TYPE(Sint08, int8_t);
            CASE_COL_TYPE(Uint16, uint16_t);
            CASE_COL_TYPE(Sint16, int16_t);
            CASE_COL_TYPE(Uint32, uint32_t);
            CASE_COL_TYPE(Sint32, int32_t);
            CASE_COL_TYPE(Uint64, uint64_t);
            CASE_COL_TYPE(Sint64, int64_t);
            CASE_COL_TYPE(Float32, float);
            CASE_COL_TYPE(Float64, double);
        }
#undef CASE_COL_TYPE
    }
    if(schema.getFixedRowLen() != 0)
    {
        return new TrbWritableIndexTemplate<void, std::true_type>(fpath, schema.getFixedRowLen(), schema.m_isUnique);
    }
    else
    {
        return new TrbWritableIndexTemplate<void, std::false_type>(fpath, schema.getFixedRowLen(), schema.m_isUnique);
    }
}

}}} //namespace terark { namespace db { namespace trbdb {