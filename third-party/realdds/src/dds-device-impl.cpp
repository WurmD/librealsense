// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include "dds-device-impl.h"

#include <realdds/dds-participant.h>
#include <realdds/dds-topic-reader.h>
#include <realdds/dds-topic-writer.h>
#include <realdds/dds-subscriber.h>
#include <realdds/topics/flexible/flexible-msg.h>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>

#include <librealsense2/utilities/time/timer.h>
#include <librealsense2/utilities/json.h>

#include <third-party/json.hpp>

#include <cassert>


using nlohmann::json;


namespace realdds {

namespace {

enum class state_type
{
    WAIT_FOR_DEVICE_HEADER,
    WAIT_FOR_PROFILES,
    DONE
    // TODO - Assuming all profiles of a stream will be sent in a single video_stream_profiles_msg
    // Otherwise need a stream header message with expected number of profiles for each stream
    // But then need all stream header messages to be sent before any profile message for a simple state machine
};

char const * to_string( state_type st )
{
    switch( st )
    {
    case state_type::WAIT_FOR_DEVICE_HEADER:
        return "WAIT_FOR_DEVICE_HEADER";
    case state_type::WAIT_FOR_PROFILES:
        return "WAIT_FOR_PROFILES";
    case state_type::DONE:
        return "DONE";
    default:
        return "UNKNOWN";
    }
}

std::ostream& operator<<( std::ostream& s, state_type st )
{
    s << to_string( st );
    return s;
}

}  // namespace


dds_device::impl::impl( std::shared_ptr< dds_participant > const & participant,
                        dds_guid const & guid,
                        topics::device_info const & info )
    : _info( info )
    , _guid( guid )
    , _participant( participant )
    , _subscriber( std::make_shared< dds_subscriber >( participant ) )
{
}

void dds_device::impl::run()
{
    if( _running )
        DDS_THROW( runtime_error, "device '" + _info.name + "' is already running" );

    create_notifications_reader();
    create_control_writer();
    if( ! init() )
        DDS_THROW( runtime_error, "failed getting stream data from '" + _info.topic_root + "'" );

    LOG_DEBUG( "device '" << _info.topic_root << "' (" << _participant->print( _guid )
                          << ") initialized successfully" );
    _running = true;
}

void dds_device::impl::open( const dds_stream_profiles & profiles )
{
    if ( profiles.empty() )
        DDS_THROW( runtime_error, "must provide at least one profile" );
    using nlohmann::json;
    auto stream_profiles = json();
    for ( auto & profile : profiles )
    {
        auto stream = profile->stream();
        if ( !stream )
            DDS_THROW( runtime_error, "profile (" + profile->to_string() + ") is not part of any stream" );
        if ( stream_profiles.find( stream->name() ) != stream_profiles.end() )
            DDS_THROW( runtime_error, "more than one profile found for stream '" + stream->name() + "'" );

        stream_profiles[stream->name()] = profile->to_json();

        _streams[stream->name()]->open( _info.topic_root + '/' + stream->name(), _subscriber );
    }

    json j = {
        { "id", "open-streams" },
        { "stream-profiles", stream_profiles },
    };

    write_control_message( j );
}

void dds_device::impl::close( dds_streams const & streams )
{
    if ( streams.empty() )
        DDS_THROW( runtime_error, "must provide at least one stream" );
    using nlohmann::json;
    auto stream_names = json::array();
    for ( auto & stream : streams )
    {
        if ( !stream )
            DDS_THROW( runtime_error, "null stream passed in" );
        stream_names += stream->name();

        _streams[stream->name()]->close();
    }

    json j = {
        { "id", "close-streams" },
        { "stream-names", stream_names },
    };

    write_control_message( j );
}

void dds_device::impl::write_control_message( topics::flexible_msg && msg )
{
    assert( _control_writer != nullptr );
    msg.write_to( *_control_writer );
}

void dds_device::impl::create_notifications_reader()
{
    if( _notifications_reader )
        return;

    auto topic = topics::flexible_msg::create_topic( _participant, _info.topic_root + "/notification" );

    _notifications_reader = std::make_shared< dds_topic_reader >( topic );
    _notifications_reader->run( dds_topic_reader::qos( eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS ) );
}

void dds_device::impl::create_control_writer()
{
    if( _control_writer )
        return;

    auto topic = topics::flexible_msg::create_topic( _participant, _info.topic_root + "/control" );
    _control_writer = std::make_shared< dds_topic_writer >( topic );
    dds_topic_writer::qos wqos( eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS );
    wqos.history().depth = 10;  // default is 1
    _control_writer->run( wqos );
}

bool dds_device::impl::init()
{
    // We expect to receive all of the sensors data under a timeout
    utilities::time::timer t( std::chrono::seconds( 30 ) );  // TODO: refine time out
    state_type state = state_type::WAIT_FOR_DEVICE_HEADER;
    std::map< std::string, int > sensor_name_to_index;
    size_t n_streams_expected = 0;
    while( ! t.has_expired() && state_type::DONE != state )
    {
        LOG_DEBUG( state << "..." );
        eprosima::fastrtps::Duration_t one_second = { 1, 0 };
        if( _notifications_reader->get()->wait_for_unread_message( one_second ) )
        {
            topics::flexible_msg notification;
            eprosima::fastdds::dds::SampleInfo info;
            while( topics::flexible_msg::take_next( *_notifications_reader, &notification, &info ) )
            {
                if( ! notification.is_valid() )
                    continue;
                auto j = notification.json_data();
                auto id = utilities::json::get< std::string >( j, "id" );
                if( state_type::WAIT_FOR_DEVICE_HEADER == state && id == "device-header" )
                {
                    n_streams_expected = utilities::json::get< size_t >( j, "n-streams" );
                    LOG_DEBUG( "... device-header: " << n_streams_expected << " streams expected" );
                    if( n_streams_expected )
                        state = state_type::WAIT_FOR_PROFILES;
                    else
                        state = state_type::DONE;
                }
                else if( state_type::WAIT_FOR_PROFILES == state && id == "stream-header" )
                {
                    if( _streams.size() >= n_streams_expected )
                        DDS_THROW( runtime_error,
                                   "more streams than expected (" + std::to_string( n_streams_expected )
                                       + ") received" );
                    auto stream_type = utilities::json::get< std::string >( j, "type" );
                    auto stream_name = utilities::json::get< std::string >( j, "name" );
                    auto & stream = _streams[stream_name];
                    if( stream )
                        DDS_THROW( runtime_error, "stream '" + stream_name + "' already exists" );
                    auto sensor_name = utilities::json::get< std::string >( j, "sensor-name" );
                    auto default_profile_index = utilities::json::get< int >( j, "default-profile-index" );
                    dds_stream_profiles profiles;

#define TYPE2STREAM( S, P )                                                                                            \
    if( stream_type == #S )                                                                                            \
    {                                                                                                                  \
        for( auto & profile : j["profiles"] )                                                                          \
            profiles.push_back( dds_stream_profile::from_json< dds_##P##_stream_profile >( profile ) );                \
        stream = std::make_shared< dds_##S##_stream >( stream_name, sensor_name );                                     \
    }                                                                                                                  \
    else

                    TYPE2STREAM( depth, video )
                    TYPE2STREAM( ir, video )
                    TYPE2STREAM( color, video )
                    TYPE2STREAM( accel, motion )
                    TYPE2STREAM( gyro, motion )
                    TYPE2STREAM( fisheye, video )
                    TYPE2STREAM( confidence, video )
                    TYPE2STREAM( pose, motion )
                        DDS_THROW( runtime_error, "stream '" + stream_name + "' is of unknown type '" + stream_type + "'" );

                    if( default_profile_index < 0 || default_profile_index >= profiles.size() )
                        DDS_THROW( runtime_error,
                                   "stream '" + stream_name + "' default profile index "
                                       + std::to_string( default_profile_index ) + " is out of bounds" );
                    if( strcmp( stream->type_string(), stream_type.c_str() ) != 0 )
                        DDS_THROW( runtime_error,
                                   "failed to instantiate stream type '" + stream_type + "' (instead, got '"
                                       + stream->type_string() + "')" );
                    stream->init_profiles( profiles, default_profile_index );
                    LOG_DEBUG( "... stream '" << stream_name << "' (" << _streams.size() << "/" << n_streams_expected
                                              << ") received with " << profiles.size() << " profiles" );
                    if( _streams.size() >= n_streams_expected )
                        state = state_type::DONE;
                }
                else
                {
                    DDS_THROW( runtime_error, "unexpected notification '" + id + "' in " + to_string( state ) );
                }
            }
        }
    }

    LOG_DEBUG( "... " << ( state_type::DONE == state ? "" : "timed out; state is " ) << state );

    return ( state_type::DONE == state );
}


}  // namespace realdds