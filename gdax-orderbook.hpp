#ifndef GDAX_ORDERBOOK_HPP
#define GDAX_ORDERBOOK_HPP

#include <iostream>
#include <string>

#include <cds/container/rwqueue.h>
#include <cds/container/skip_list_map_hp.h>
#include <cds/gc/hp.h>
#include <cds/init.h>

#include <rapidjson/document.h>

#include <websocketpp/client.hpp>
#include <websocketpp/concurrency/none.hpp>
#include <websocketpp/config/asio_client.hpp>

/**
 * A copy of the GDAX order book for the product given during construction,
 * exposed as two maps, one for bids and one for asks, each mapping price
 * levels to order quantities, continually updated in real time via the
 * `level2` channel of the Websocket feed of the GDAX API.
 *
 * Spawns two threads, one to receive updates from the GDAX WebSocket Feed and
 * store them in an internal queue, and another to pull updates out of that
 * queue and store them in the maps.
 *
 * Queue-then-map approach chosen based on the assumption that a queue is
 * faster than a map, so updates can be pulled off the wire as fast as
 * possible, with neither map insertion latency nor map garbage collection
 * slowing down the reception pipeline.  (Future improvement: profile queue
 * usage; if consistently empty, consider going straight from WebSocket to map;
 * if consistenly used, consider allowing configuration of queue size in order
 * to avoid overflow.)
 *
 * To ensure high performance, implemented using concurrent data structures
 * from libcds.  The internal queue is a cds::container::RWQueue, whose doc
 * says "The queue has two different locks: one for reading and one for
 * writing. Therefore, one writer and one reader can simultaneously access to
 * the queue."  The use case in this implementation has exactly one reader
 * thread and one writer thread.  The price->quantity maps are instances of
 * cds::container::SkipListMap, whose doc says it is lock-free.
 */
class GDAXOrderBook {
private:
    struct CDSInitializer {
        CDSInitializer() { cds::Initialize(); }
        ~CDSInitializer() { cds::Terminate(); }
    } m_cdsInitializer;

    cds::gc::HP m_cdsGarbageCollector;

public:
    GDAXOrderBook(const std::string product = "BTC-USD")
        : m_cdsGarbageCollector(67*2),
            // per SkipListMap doc, 67 hazard pointers per instance
          receiveUpdatesThread(&GDAXOrderBook::receiveUpdates, this, product),
          processUpdatesThread(&GDAXOrderBook::processUpdates, this)
    {
        // allow constructing thread to interact with libcds maps:
        if (cds::threading::Manager::isThreadAttached() == false)
            cds::threading::Manager::attachThread();

        receiveUpdatesThread.detach();
        processUpdatesThread.detach();

        while ( ! m_bookInitialized ) { continue; }
    }

    using Price = double;
    using Size = double;
    using map_t = cds::container::SkipListMap<cds::gc::HP, Price, Size>;
    // map_t::get(Price) returns an std::pair<Price, Size>*
    map_t bids;
    map_t asks;

    ~GDAXOrderBook()
    {
        // tell threads we're terminating, and wait for them to finish
        m_client.stop(); // signal to WebSocket thread
        m_stopUpdating = true; // signal to book update thread
        while ( processUpdatesThread.joinable()
             && receiveUpdatesThread.joinable() )
        {
            continue;
        }
    }

private:
    std::thread receiveUpdatesThread;
    std::thread processUpdatesThread;

    cds::container::RWQueue<std::string> m_queue;

    struct websocketppPolicy
        : public websocketpp::config::asio_tls_client
        // would prefer transport::iostream instead of transport::asio,
        // because we only have one thread using the WebSocket, but the only
        // policy preconfigured with TLS support is the asio one.
    {
        typedef websocketpp::concurrency::none concurrency_type;
    };
    using WebSocketClient = websocketpp::client<websocketppPolicy>;
    WebSocketClient m_client;

    /**
     * Initiates WebSocket connection, subscribes to order book updates for the
     * given product, installs a message handler which will receive updates
     * and enqueue them to m_queue, and starts the asio event loop.
     */
    void receiveUpdates(std::string product)
    {
        try {
            m_client.clear_access_channels(websocketpp::log::alevel::all);
            m_client.set_access_channels(
                websocketpp::log::alevel::connect |
                websocketpp::log::alevel::disconnect);

            m_client.clear_error_channels(websocketpp::log::elevel::all);
            m_client.set_error_channels(
                websocketpp::log::elevel::info |
                websocketpp::log::elevel::warn |
                websocketpp::log::elevel::rerror |
                websocketpp::log::elevel::fatal);

            m_client.init_asio();

            m_client.set_message_handler(
                [this](websocketpp::connection_hdl,
                       websocketppPolicy::message_type::ptr msg)
                {
                    this->m_queue.enqueue(msg->get_payload());
                });

            m_client.set_tls_init_handler(
                [](websocketpp::connection_hdl)
                {
                    websocketpp::lib::shared_ptr<boost::asio::ssl::context>
                        context = websocketpp::lib::make_shared<
                            boost::asio::ssl::context>(
                            boost::asio::ssl::context::tlsv1);

                    try {
                        context->set_options(
                            boost::asio::ssl::context::default_workarounds |
                            boost::asio::ssl::context::no_sslv2 |
                            boost::asio::ssl::context::no_sslv3 |
                            boost::asio::ssl::context::single_dh_use);
                    } catch (std::exception& e) {
                        std::cerr << "set_tls_init_handler() failed to set"
                            " context options: " << e.what() << std::endl;
                    }
                    return context;
                });

            m_client.set_open_handler(
                [product, this](websocketpp::connection_hdl handle)
                {
                    // subscribe to updates to product's order book
                    websocketpp::lib::error_code errorCode;
                    this->m_client.send(handle,
                        "{"
                            "\"type\": \"subscribe\","
                            "\"product_ids\": [" "\""+product+"\"" "],"
                            "\"channels\": [" "\"level2\"" "]"
                        "}", websocketpp::frame::opcode::text, errorCode);
                    if (errorCode) {
                        std::cerr << "error sending subscription: " +
                            errorCode.message() << std::endl;
                    }

                    if (cds::threading::Manager::isThreadAttached() == false)
                        cds::threading::Manager::attachThread();
                });

            websocketpp::lib::error_code errorCode;
            auto connection =
                m_client.get_connection("wss://ws-feed.gdax.com", errorCode);
            if (errorCode) {
                std::cerr << "failed WebSocketClient::get_connection(): " <<
                    errorCode.message() << std::endl;
            }

            m_client.connect(connection);

            m_client.run();
        } catch (websocketpp::exception const & e) {
            std::cerr << "receiveUpdates() failed: " << e.what() << std::endl;
        }
    }

    bool m_stopUpdating = false; // for termination of processUpdates() thread
    bool m_bookInitialized = false; // to tell constructor to return

    /**
     * Continually dequeues order book updates from `m_queue` (busy-waiting if
     * there aren't any), and moves those updates to `bids` and `asks`, until
     * `m_stopUpdating` is true.
     */
    void processUpdates()
    {
        if (cds::threading::Manager::isThreadAttached() == false)
            cds::threading::Manager::attachThread();

        while ( true ) 
        {
            if ( m_stopUpdating ) return;

            std::string update;
            bool queueEmpty = ! m_queue.dequeue(update);
            if (queueEmpty) continue;

            rapidjson::Document doc;
            doc.Parse(update.c_str());

            using std::stod;

            std::string type(doc["type"].GetString());
            if ( type == "snapshot" )
            {
                for (auto i : { std::make_pair("bids", &bids),
                                std::make_pair("asks", &asks)  } )
                {
                    for (auto j = 0 ; j < doc[i.first].Size() ; ++j)
                    {
                        Price price = stod(doc[i.first][j][0].GetString());
                        Size   size = stod(doc[i.first][j][1].GetString());

                        i.second->insert(price, size);
                    }
                }

                m_bookInitialized = true;
            }
            else if ( type == "l2update" )
            {
                for (auto i = 0 ; i < doc["changes"].Size() ; ++i)
                {
                    std::string buyOrSell(doc["changes"][i][0].GetString()),
                                price    (doc["changes"][i][1].GetString()),
                                size     (doc["changes"][i][2].GetString());

                    map_t * map = buyOrSell=="buy" ? &bids : &asks;

                    if (stod(size) == 0) { map->erase(stod(price)); }
                    else
                    {
                        map->update(
                            stod(price),
                            [&doc, size](bool & bNew,
                                       std::pair<const Price, Size> & pair)
                            {
                                pair.second = stod(size);
                            });
                    }
                }
            }
        }
    }
};

#endif // GDAX_ORDERBOOK_HPP