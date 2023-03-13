// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include "lrs-device-controller.h"

#include <common/metadata-helper.h>

#include <rsutils/easylogging/easyloggingpp.h>
#include <rsutils/json.h>

#include <realdds/topics/flexible/flexible-msg.h>
#include <realdds/dds-device-server.h>
#include <realdds/dds-stream-server.h>

#include <algorithm>
#include <iostream>

using nlohmann::json;
using namespace realdds;
using tools::lrs_device_controller;


#define CREATE_SERVER_IF_NEEDED( X )                                                                                   \
    if( server )                                                                                                       \
    {                                                                                                                  \
        if( strcmp( server->type_string(), #X ) )                                                                      \
        {                                                                                                              \
            LOG_ERROR( #X " profile type on a stream '" << stream_name << "' that already has type "                   \
                                                        << server->type_string() );                                    \
            return;                                                                                                    \
        }                                                                                                              \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        server = std::make_shared< realdds::dds_##X##_stream_server >( stream_name, sensor_name );                     \
    }                                                                                                                  \
    break

realdds::dds_option_range to_realdds( const rs2::option_range & range )
{
    realdds::dds_option_range ret_val;
    ret_val.max = range.max;
    ret_val.min = range.min;
    ret_val.step = range.step;
    ret_val.default_value = range.def;

    return ret_val;
}

realdds::video_intrinsics to_realdds( const rs2_intrinsics & intr )
{
    realdds::video_intrinsics ret;

    ret.width = intr.width;
    ret.height = intr.height;
    ret.principal_point_x = intr.ppx;
    ret.principal_point_y = intr.ppy;
    ret.focal_lenght_x = intr.fx;
    ret.focal_lenght_y = intr.fy;
    ret.distortion_model = intr.model;
    memcpy( ret.distortion_coeffs.data(), intr.coeffs, sizeof( ret.distortion_coeffs ) );

    return ret;
}

realdds::motion_intrinsics to_realdds( const rs2_motion_device_intrinsic & rs2_intr )
{
    realdds::motion_intrinsics intr;

    memcpy( intr.data.data(), rs2_intr.data, sizeof( intr.data ) );
    memcpy( intr.noise_variances.data(), rs2_intr.noise_variances, sizeof( intr.noise_variances ) );
    memcpy( intr.bias_variances.data(), rs2_intr.bias_variances, sizeof( intr.bias_variances ) );

    return intr;
}

realdds::extrinsics to_realdds( const rs2_extrinsics & rs2_extr )
{
    realdds::extrinsics extr;

    memcpy( extr.rotation.data(), rs2_extr.rotation, sizeof( extr.rotation ) );
    memcpy( extr.translation.data(), rs2_extr.translation, sizeof( extr.translation ) );

    return extr;
}


static std::string stream_name_from_rs2( const rs2::stream_profile & profile )
{
    // ROS stream names cannot contain spaces! We use underscores instead:
    std::string stream_name = rs2_stream_to_string( profile.stream_type() );
    if( profile.stream_index() )
        stream_name += '_' + std::to_string( profile.stream_index() );
    return stream_name;
}


std::vector< std::shared_ptr< realdds::dds_stream_server > > lrs_device_controller::get_supported_streams()
{
    std::map< std::string, realdds::dds_stream_profiles > stream_name_to_profiles;
    std::map< std::string, int > stream_name_to_default_profile;
    std::map< std::string, std::set< realdds::video_intrinsics > > stream_name_to_video_intrinsics;
    std::map< std::string, realdds::motion_intrinsics > stream_name_to_motion_intrinsics;

    // Iterate over all profiles of all sensors and build appropriate dds_stream_servers
    for( auto sensor : _rs_dev.query_sensors() )
    {
        std::string const sensor_name = sensor.get_info( RS2_CAMERA_INFO_NAME );
        // We keep a copy of the sensors throughout the run time:
        // Otherwise problems could arise like opening streams and they would close at start_streaming scope end.
        _rs_sensors[sensor_name] = sensor;

        auto stream_profiles = sensor.get_stream_profiles();
        std::for_each( stream_profiles.begin(), stream_profiles.end(), [&]( const rs2::stream_profile & sp )
        {
            std::string stream_name = stream_name_from_rs2( sp );

            //Create a realdds::dds_stream_server object for each unique profile type+index
            auto & server = _stream_name_to_server[stream_name];
            switch( sp.stream_type() )
            {
            case RS2_STREAM_DEPTH: CREATE_SERVER_IF_NEEDED( depth );
            case RS2_STREAM_INFRARED: CREATE_SERVER_IF_NEEDED( ir );
            case RS2_STREAM_COLOR: CREATE_SERVER_IF_NEEDED( color );
            case RS2_STREAM_FISHEYE: CREATE_SERVER_IF_NEEDED( fisheye );
            case RS2_STREAM_CONFIDENCE: CREATE_SERVER_IF_NEEDED( confidence );
            case RS2_STREAM_ACCEL: CREATE_SERVER_IF_NEEDED( accel );
            case RS2_STREAM_GYRO: CREATE_SERVER_IF_NEEDED( gyro );
            case RS2_STREAM_POSE: CREATE_SERVER_IF_NEEDED( pose );
            default:
                LOG_ERROR( "unsupported stream type " << sp.stream_type() );
                return;
            }

            // Create appropriate realdds::profile for each sensor profile and map to a stream
            auto & profiles = stream_name_to_profiles[stream_name];
            std::shared_ptr< realdds::dds_stream_profile > profile;
            if( sp.is< rs2::video_stream_profile >() )
            {
                auto const & vsp = sp.as< rs2::video_stream_profile >();
                profile = std::make_shared< realdds::dds_video_stream_profile >(
                    static_cast< int16_t >( vsp.fps() ),
                    realdds::dds_stream_format::from_rs2( vsp.format() ),
                    static_cast< uint16_t >( vsp.width() ),
                    static_cast< int16_t >( vsp.height() ) );
                try
                {
                    auto intr = to_realdds( vsp.get_intrinsics() );
                    stream_name_to_video_intrinsics[stream_name].insert( intr );
                }
                catch( ... ) {} //Some profiles don't have intrinsics
            }
            else if( sp.is< rs2::motion_stream_profile >() )
            {
                const auto & msp = sp.as< rs2::motion_stream_profile >();
                profile = std::make_shared< realdds::dds_motion_stream_profile >(
                    static_cast< int16_t >( msp.fps() ),
                    realdds::dds_stream_format::from_rs2( msp.format() ) );

                stream_name_to_motion_intrinsics[stream_name] = to_realdds( msp.get_motion_intrinsics() );
            }
            else
            {
                LOG_ERROR( "unknown profile type of uid " << sp.unique_id() );
                return;
            }
            if( sp.is_default() )
                stream_name_to_default_profile[stream_name] = static_cast< int >( profiles.size() );
            profiles.push_back( profile );
            LOG_DEBUG( stream_name << ": " << profile->to_string() );
        } );
    }

    // Iterate over the mapped streams and initialize
    std::vector< std::shared_ptr< realdds::dds_stream_server > > servers;
    for( auto & it : stream_name_to_profiles )
    {
        auto const & stream_name = it.first;

        int default_profile_index = 0;
        auto default_profile_it = stream_name_to_default_profile.find( stream_name );
        if( default_profile_it != stream_name_to_default_profile.end() )
            default_profile_index = default_profile_it->second;

        auto const & profiles = it.second;
        if( profiles.empty() )
        {
            LOG_ERROR( "ignoring stream '" << stream_name << "' with no profiles" );
            continue;
        }
        auto server = _stream_name_to_server[stream_name];
        if( ! server )
        {
            LOG_ERROR( "ignoring stream '" << stream_name << "' with no server" );
            continue;
        }

        // Set stream metadata support (currently if the device supports metadata all streams does)
        // Must be done before calling init_profiles()
        if( _md_enabled )
        {
            server->enable_metadata();
        }

        server->init_profiles( profiles, default_profile_index );

        // Set stream intrinsics
        auto video_server = std::dynamic_pointer_cast< dds_video_stream_server >( server );
        auto motion_server = std::dynamic_pointer_cast< dds_motion_stream_server >( server );
        if( video_server )
        {
            video_server->set_intrinsics( std::move( stream_name_to_video_intrinsics[stream_name] ) );
        }
        if( motion_server )
        {
            motion_server->set_intrinsics( std::move( stream_name_to_motion_intrinsics[stream_name] ) );
        }

        realdds::dds_options options;
        // Get supported options for this stream
        for( auto sensor : _rs_dev.query_sensors() )
        {
            std::string const sensor_name = sensor.get_info( RS2_CAMERA_INFO_NAME );
            if( server->sensor_name().compare( sensor_name ) == 0 )
            {
                auto supported_options = sensor.get_supported_options();
                // Hack - some options can be queried only if streaming so start sensor and close after query
                // sensor.open( sensor.get_stream_profiles()[0] );
                // sensor.start( []( rs2::frame f ) {} );
                for( auto option : supported_options )
                {
                    auto dds_opt = std::make_shared< realdds::dds_option >( std::string( sensor.get_option_name( option ) ),
                                                                            server->name() );
                    try
                    {
                        dds_opt->set_value( sensor.get_option( option ) );
                        dds_opt->set_range( to_realdds( sensor.get_option_range( option ) ) );
                        dds_opt->set_description( sensor.get_option_description( option ) );
                    }
                    catch( ... )
                    {
                        LOG_ERROR( "Cannot query details of option " << option );
                        continue;  // Some options can be queried only if certain conditions exist skip them for now
                    }
                    options.push_back( dds_opt );  // TODO - filter options relevant for stream type
                }
                // sensor.stop();
                // sensor.close();
            }
        }
        server->init_options( options );

        servers.push_back( server );
    }

    return servers;
}

#undef CREATE_SERVER_IF_NEEDED

extrinsics_map get_extrinsics_map( const rs2::device & dev )
{
    extrinsics_map ret;
    std::map< std::string, rs2::stream_profile > stream_name_to_rs2_stream_profile;

    // Iterate over profiles of all sensors and split to streams
    for( auto sensor : dev.query_sensors() )
    {
        auto stream_profiles = sensor.get_stream_profiles();
        std::for_each( stream_profiles.begin(), stream_profiles.end(), [&]( const rs2::stream_profile & sp ) {
            std::string stream_name = sp.stream_name();
            if( stream_name_to_rs2_stream_profile.count( stream_name ) == 0 )
                stream_name_to_rs2_stream_profile[stream_name] = sp; // Any profile of this stream will do, take the first
        } );
    }

    // For each stream, get extrinsics to all other streams
    for( auto & from : stream_name_to_rs2_stream_profile )
    {
        auto const & from_stream_name = from.first;
        for( auto & to : stream_name_to_rs2_stream_profile )
        {
            auto & to_stream_name = to.first;
            if( from_stream_name != to_stream_name )
            {
                // Get rs2::stream_profile objects for get_extrinsics API call
                const rs2::stream_profile & from_profile = from.second;
                const rs2::stream_profile & to_profile = to.second;
                const auto & extrinsics = from_profile.get_extrinsics_to( to_profile );
                ret[std::make_pair( from_stream_name, to_stream_name )] =
                    std::make_shared< realdds::extrinsics >( to_realdds( extrinsics ) );
            }
        }
    }

    return ret;
}


std::shared_ptr< dds_stream_profile > create_dds_stream_profile( rs2_stream type, nlohmann::json const & j )
{
    switch( type )
    {
    case RS2_STREAM_DEPTH:
    case RS2_STREAM_COLOR:
    case RS2_STREAM_INFRARED:
    case RS2_STREAM_FISHEYE:
    case RS2_STREAM_CONFIDENCE:
        return dds_stream_profile::from_json< dds_video_stream_profile >( j );

    case RS2_STREAM_GYRO:
    case RS2_STREAM_ACCEL:
    case RS2_STREAM_POSE:
        return dds_stream_profile::from_json< dds_motion_stream_profile >( j );
    }

    throw std::runtime_error( "Unsupported stream type" );
}

rs2_stream stream_name_to_type( std::string const & type_string )
{
    static const std::map< std::string, rs2_stream > type_to_rs2 = {
        { "Depth", RS2_STREAM_DEPTH },
        { "Color", RS2_STREAM_COLOR },
        { "Infrared", RS2_STREAM_INFRARED },
        { "Infrared_1", RS2_STREAM_INFRARED },
        { "Infrared_2", RS2_STREAM_INFRARED },
        { "Fisheye", RS2_STREAM_FISHEYE },
        { "Gyro", RS2_STREAM_GYRO },
        { "Accel", RS2_STREAM_ACCEL },
        { "Gpio", RS2_STREAM_GPIO },
        { "Pose", RS2_STREAM_POSE },
        { "Confidence", RS2_STREAM_CONFIDENCE },
    };
    auto it = type_to_rs2.find( type_string );
    if( it == type_to_rs2.end() )
    {
        LOG_ERROR( "Unknown stream type '" << type_string << "'" );
        return RS2_STREAM_ANY;
    }
    return it->second;
}

rs2_stream type_string_to_rs2_stream( std::string const & type_string )
{
    static const std::map< std::string, rs2_stream > type_to_rs2 = {
        { "depth", RS2_STREAM_DEPTH },
        { "color", RS2_STREAM_COLOR },
        { "ir", RS2_STREAM_INFRARED },
        { "fisheye", RS2_STREAM_FISHEYE },
        { "gyro", RS2_STREAM_GYRO },
        { "accel", RS2_STREAM_ACCEL },
        { "pose", RS2_STREAM_POSE },
        { "confidence", RS2_STREAM_CONFIDENCE },
    };
    auto it = type_to_rs2.find( type_string );
    if( it == type_to_rs2.end() )
    {
        LOG_ERROR( "Unknown stream type '" << type_string << "'" );
        return RS2_STREAM_ANY;
    }
    return it->second;
}

int stream_name_to_index( std::string const & type_string )
{
    int index = 0;
    static const std::map< std::string, int > type_to_index = {
        { "Infrared_1", 1 },
        { "Infrared_2", 2 },
    };
    auto it = type_to_index.find( type_string );
    if( it != type_to_index.end() )
    {
        index = it->second;
    }

    return index;
}

rs2_option option_name_to_type( const std::string & name, const rs2::sensor & sensor )
{
    for( size_t i = 0; i < static_cast< size_t >( RS2_OPTION_COUNT ); i++ )
    {
        if( name.compare( sensor.get_option_name( static_cast< rs2_option >( i ) ) ) == 0 )
        {
            return static_cast< rs2_option >( i );
        }
    }

    throw std::runtime_error( "Option '" + name + "' type not found" );
}


bool profiles_are_compatible( std::shared_ptr< dds_stream_profile > const & p1,
                              std::shared_ptr< dds_stream_profile > const & p2,
                              bool any_format = false )
{
    auto vp1 = std::dynamic_pointer_cast< realdds::dds_video_stream_profile >( p1 );
    auto vp2 = std::dynamic_pointer_cast< realdds::dds_video_stream_profile >( p2 );
    if( ! ! vp1 != ! ! vp2 )
        return false;  // types aren't the same
    if( vp1 && vp2 )
        if( vp1->width() != vp2->width() || vp1->height() != vp2->height() )
            return false;
    if( ! any_format && p1->format() != p2->format() )
        return false;
    return p1->frequency() == p2->frequency();
}


rs2::stream_profile get_required_profile( const rs2::sensor & sensor,
                                          std::string const & stream_name,
                                          std::shared_ptr< dds_stream_profile > const & profile )
{
    auto sensor_stream_profiles = sensor.get_stream_profiles();
    auto profile_iter = std::find_if( sensor_stream_profiles.begin(),
                                      sensor_stream_profiles.end(),
                                      [&]( rs2::stream_profile sp ) {
                                          auto vp = sp.as< rs2::video_stream_profile >();
                                          auto dds_vp = std::dynamic_pointer_cast< dds_video_stream_profile >( profile );
                                          bool video_params_match = ( vp && dds_vp ) ?
                                              vp.width() == dds_vp->width() && vp.height() == dds_vp->height() : true;
                                          return sp.stream_type() == stream_name_to_type( stream_name )
                                              && sp.stream_index() == stream_name_to_index( stream_name )
                                              && sp.fps() == profile->frequency()
                                              && sp.format() == profile->format().to_rs2()
                                              && video_params_match;
                                      } );
    if( profile_iter == sensor_stream_profiles.end() )
    {
        throw std::runtime_error( "Could not find required profile" );
    }

    return *profile_iter;
}


static std::shared_ptr< realdds::dds_stream_profile >
find_profile( std::shared_ptr< realdds::dds_stream_server > const & stream,
              std::shared_ptr< realdds::dds_stream_profile > const & profile,
              bool any_format = false )
{
    auto & stream_profiles = stream->profiles();
    auto it = std::find_if( stream_profiles.begin(),
                            stream_profiles.end(),
                            [&]( std::shared_ptr< realdds::dds_stream_profile > const & sp )
                            {
                                return profiles_are_compatible( sp, profile, any_format );
                            } );
    std::shared_ptr< realdds::dds_stream_profile > found_profile;
    if( it != stream_profiles.end() )
        found_profile = *it;
    return found_profile;
}


lrs_device_controller::lrs_device_controller( rs2::device dev, std::shared_ptr< realdds::dds_device_server > dds_device_server )
    : _rs_dev( dev )
    , _dds_device_server( dds_device_server )
{
    if( ! _dds_device_server )
        throw std::runtime_error( "Empty dds_device_server" );

    _dds_device_server->on_open_streams( [&]( const json & msg ) { start_streaming( msg ); } );
    _dds_device_server->on_set_option( [&]( const std::shared_ptr< realdds::dds_option > & option, float value ) {
        set_option( option, value );
    } );
    _dds_device_server->on_query_option( [&]( const std::shared_ptr< realdds::dds_option > & option ) -> float {
        return query_option( option );
    } );

    _device_sn = _rs_dev.get_info( RS2_CAMERA_INFO_SERIAL_NUMBER );
    LOG_DEBUG( "LRS device manager for device: " << _device_sn << " created" );

    // Some camera models support metadata for frames. can_support_metadata will tell us if this model does.
    // Also, to get the metadata driver support needs to be enabled, requires administrator rights on Windows and Linux.
    // is_enabled will return current state. If one of the conditions is false we cannot get metadata from the device.
    _md_enabled = rs2::metadata_helper::instance().can_support_metadata( _rs_dev.get_info( RS2_CAMERA_INFO_PRODUCT_LINE ) )
               && rs2::metadata_helper::instance().is_enabled( _rs_dev.get_info( RS2_CAMERA_INFO_PHYSICAL_PORT ) );

    // Create a supported streams list for initializing the relevant DDS topics
    auto supported_streams = get_supported_streams();

    _bridge.on_start_sensor( [this]( std::string const & sensor_name, dds_stream_profiles const & active_profiles ) {
        auto & sensor = _rs_sensors[sensor_name];
        auto rs2_profiles = get_rs2_profiles( active_profiles );
        sensor.open( rs2_profiles );
        sensor.start( [this]( rs2::frame f ) {
            auto stream_name = stream_name_from_rs2( f.get_profile() );
            auto it = _stream_name_to_server.find( stream_name );
            if( it != _stream_name_to_server.end() )
            {
                auto & server = it->second;
                if( _bridge.is_streaming( server ) )
                {
                    server->publish( static_cast< const uint8_t * >( f.get_data() ), f.get_data_size(), f.get_frame_number() );
                    publish_frame_metadata( f );
                }
            }
        } );
        std::cout << sensor_name << " sensor started" << std::endl;
    } );
    _bridge.on_stop_sensor( [this]( std::string const & sensor_name ) {
        auto & sensor = _rs_sensors[sensor_name];
        sensor.stop();
        sensor.close();
        std::cout << sensor_name << " sensor stopped" << std::endl;
    } );
    _bridge.on_error( [this]( std::string const & error_string ) {
        nlohmann::json j = nlohmann::json::object( {
            { "id", "error" },
            { "error", error_string },
        } );
        _dds_device_server->publish_notification( std::move( j ) );
    } );
    _bridge.init( supported_streams );

    auto extrinsics = get_extrinsics_map( dev );

    realdds::dds_options options;  // TODO - get all device level options

    // Initialize the DDS device server with the supported streams
    _dds_device_server->init( supported_streams, options, extrinsics );
}


lrs_device_controller::~lrs_device_controller()
{
    LOG_DEBUG( "LRS device manager for device: " << _device_sn << " deleted" );
}


void lrs_device_controller::start_streaming( const json & msg )
{
    // Note that this function is called "start-streaming" but it's really a response to "open-streams" so does not
    // actually start streaming. It simply sets and locks in which streams should be open when streaming starts.
    // This effectively lets one control _specifically_ which streams should be streamable, and nothing else: if left
    // out, a sensor is reset back to its default state using implicit stream selection.
    // (For example, the 'Stereo Module' sensor controls Depth, IR1, IR2: but turning on all 3 has performance
    // implications and may not be desirable. So you can open only Depth and IR1/2 will stay inactive...)
    if( rsutils::json::get< bool >( msg, "reset", true ) )
        _bridge.reset();

    auto const & msg_profiles = msg["stream-profiles"];
    for( auto const & name2profile : msg_profiles.items() )
    {
        std::string const & stream_name = name2profile.key();
        auto name2server = _stream_name_to_server.find( stream_name );
        if( name2server == _stream_name_to_server.end() )
            throw std::runtime_error( "invalid stream name '" + stream_name + "'" );
        auto server = name2server->second;

        auto requested_profile
            = create_dds_stream_profile( type_string_to_rs2_stream( server->type_string() ), name2profile.value() );
        auto profile = find_profile( server, requested_profile );
        if( ! profile )
            throw std::runtime_error( "invalid profile " + requested_profile->to_string() + " for stream '"
                                      + stream_name + "'" );

        _bridge.open( profile );
    }

    // We're here so all the profiles were acceptable; lock them in -- with no implicit profiles!
    if( rsutils::json::get< bool >( msg, "commit", true ) )
        _bridge.commit();
}


void lrs_device_controller::publish_frame_metadata( const rs2::frame & f )
{
    nlohmann::json md_header = nlohmann::json::object( {
        { "frame-id", std::to_string( f.get_frame_number() ) },
        { "timestamp", f.get_timestamp() },
        { "timestamp-domain", f.get_frame_timestamp_domain() }
    } );
    if( f.is< rs2::depth_frame >() )
        md_header["depth-units"] = f.as< rs2::depth_frame >().get_units();

    nlohmann::json metadata = nlohmann::json::object( {} );
    for( size_t i = 0; i < static_cast< size_t >( RS2_FRAME_METADATA_COUNT ); ++i )
    {
        rs2_frame_metadata_value val = static_cast< rs2_frame_metadata_value >( i );
        if( f.supports_frame_metadata( val ) )
            metadata[rs2_frame_metadata_to_string( val )] = f.get_frame_metadata( val );
    }

    nlohmann::json md_msg = nlohmann::json::object( {
        { "stream-name", stream_name_from_rs2( f.get_profile() ) },
        { "header", md_header },
        { "metadata", metadata },
    } );
    _dds_device_server->publish_metadata( std::move( md_msg ) );
}


std::vector< rs2::stream_profile >
lrs_device_controller::get_rs2_profiles( realdds::dds_stream_profiles const & dds_profiles ) const
{
    std::vector< rs2::stream_profile > rs_profiles;
    for( auto & dds_profile : dds_profiles )
    {
        std::string stream_name = dds_profile->stream()->name();
        std::string sensor_name = dds_profile->stream()->sensor_name();

        auto it = _rs_sensors.find( sensor_name );
        if( it == _rs_sensors.end() )
        {
            LOG_ERROR( "invalid sensor name '" << sensor_name << "'" );
            continue;
        }
        auto & sensor = it->second;
        auto rs2_profile = get_required_profile( sensor, stream_name, dds_profile );
        rs_profiles.push_back( rs2_profile );
    }
    return rs_profiles;
}


void lrs_device_controller::set_option( const std::shared_ptr< realdds::dds_option > & option, float new_value )
{
    auto it = _stream_name_to_server.find( option->owner_name() );
    if( it == _stream_name_to_server.end() )
        throw std::runtime_error( "no stream '" + option->owner_name() + "' in device" );
    auto server = it->second;
    auto & sensor = _rs_sensors[server->sensor_name()];
    rs2_option opt_type = option_name_to_type( option->get_name(), sensor );
    sensor.set_option( opt_type, new_value );
}


float lrs_device_controller::query_option( const std::shared_ptr< realdds::dds_option > & option )
{
    auto it = _stream_name_to_server.find( option->owner_name() );
    if( it == _stream_name_to_server.end() )
        throw std::runtime_error( "no stream '" + option->owner_name() + "' in device" );
    auto server = it->second;
    auto & sensor = _rs_sensors[server->sensor_name()];
    rs2_option opt_type = option_name_to_type( option->get_name(), sensor );
    return sensor.get_option( opt_type );
}