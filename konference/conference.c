/*
 * app_konference
 *
 * A channel independent conference application for Asterisk
 *
 * Copyright (C) 2002, 2003 Junghanns.NET GmbH
 * Copyright (C) 2003, 2004 HorizonLive.com, Inc.
 * Copyright (C) 2005, 2005 Vipadia Limited
 * Copyright (C) 2005, 2006 HorizonWimba, Inc.
 * Copyright (C) 2007 Wimba, Inc.
 *
 * This program may be modified and distributed under the
 * terms of the GNU General Public License. You should have received
 * a copy of the GNU General Public License along with this
 * program; if not, write to the Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "asterisk/autoconfig.h"
#include "conference.h"
#include "frame.h"
#include "asterisk/utils.h"

#include "asterisk/app.h"
#include "asterisk/say.h"

#include "asterisk/musiconhold.h"

//
// static variables
//

// single-linked list of current conferences
struct ast_conference *conflist = NULL ;
#ifdef	CACHE_CONTROL_BLOCKS
struct ast_conference *confblocklist = NULL ;
#endif

// mutex for synchronizing access to conflist
//static ast_mutex_t conflist_lock = AST_MUTEX_INITIALIZER ;
AST_MUTEX_DEFINE_STATIC(conflist_lock);

static int conference_count = 0 ;

// Forward funtcion declarations
static void add_milliseconds( struct timeval* tv, long ms ) ;
static struct ast_conference* find_conf(const char* name);
static struct ast_conference* create_conf(char* name, struct ast_conf_member* member);
struct ast_conference* remove_conf(struct ast_conference* conf);
static void add_member(struct ast_conf_member* member, struct ast_conference* conf);

//
// main conference function
//
#ifdef	ONEMIXTHREAD
static void conference_exec()
{
	struct ast_conference *conf = NULL ;
#else
static void conference_exec( struct ast_conference *conf )
{
#endif
	struct ast_conf_member *member;
#if	DTMF
	struct conf_frame *cfr;
#endif
#ifdef	DTMF
	struct ast_conf_member *dtmf_source_member;
#endif
	struct conf_frame *spoken_frames, *send_frames;

	// count number of speakers, number of listeners
	int speaker_count ;
	int listener_count ;

	DEBUG("enter conference_exec\n") ;

	// timer timestamps
	struct timeval base, curr ;
	base = ast_tvnow();

	// holds differences of curr and base
	long time_diff = 0 ;
	long time_sleep = 0 ;
#ifdef	APP_KONFERENCE_DEBUG
	int since_last_slept = 0 ;
#endif
	//
	// variables for checking thread frequency
	//

	// count to AST_CONF_FRAMES_PER_SECOND
	int tf_count = 0 ;
	long tf_diff = 0 ;
	float tf_frequency = 0.0 ;

	struct timeval tf_base, tf_curr ;
	tf_base = ast_tvnow();

	int res;

	//
	// main conference thread loop
	//

	while ( 42 == 42 )
	{
		// update the current timestamp
		curr = ast_tvnow();

		// calculate difference in timestamps
		time_diff = ast_tvdiff_ms(curr, base);

		// calculate time we should sleep
		time_sleep = AST_CONF_FRAME_INTERVAL - time_diff ;

		if ( time_sleep > 0 )
		{
			// sleep for sleep_time ( as milliseconds )
			usleep( time_sleep * 1000 ) ;
#ifdef	APP_KONFERENCE_DEBUG
			// reset since last slept counter
			since_last_slept = 0 ;
#endif
			continue ;
		}
#ifdef	APP_KONFERENCE_DEBUG
		else
		{
			// long sleep warning
			if (
				!since_last_slept
				&& time_diff > AST_CONF_CONFERENCE_SLEEP * 2
			)
			{
				DEBUG("long scheduling delay, time_diff => %ld, AST_CONF_FRAME_INTERVAL => %d\n", time_diff, AST_CONF_FRAME_INTERVAL) ;
			}

			// increment times since last slept
			++since_last_slept ;

			// sleep every other time
			if ( since_last_slept % 2 )
				usleep( 0 ) ;
		}
#endif
		// adjust the timer base ( it will be used later to timestamp outgoing frames )
		add_milliseconds( &base, AST_CONF_FRAME_INTERVAL ) ;

		//
		// check thread frequency
		//


		if ( ++tf_count >= AST_CONF_FRAMES_PER_SECOND )
		{
			// update current timestamp
			tf_curr = ast_tvnow();

			// compute timestamp difference
			tf_diff = ast_tvdiff_ms(tf_curr, tf_base);

			// compute sampling frequency
			tf_frequency = ( float )( tf_diff ) / ( float )( tf_count ) ;

			if (
				( tf_frequency <= ( float )( AST_CONF_FRAME_INTERVAL - 1 ) )
				|| ( tf_frequency >= ( float )( AST_CONF_FRAME_INTERVAL + 1 ) )
			)
			{
				ast_log(
					LOG_WARNING,
					"processed frame frequency variation, tf_count => %d, tf_diff => %ld, tf_frequency => %2.4f\n",
						tf_count, tf_diff, tf_frequency
				) ;
			}

			// reset values
			tf_base = tf_curr ;
			tf_count = 0 ;
		}

		//-----------------//
		// INCOMING FRAMES //
		//-----------------//
#ifdef	ONEMIXTHREAD	
		//
		// get the first conference
		//

		if ( !(res = ast_mutex_trylock(&conflist_lock) )  ) {
			conf = conflist ;
			ast_mutex_unlock(&conflist_lock) ;
		}
#ifdef	APP_KONFERENCE_DEBUG
		else {
			DEBUG("conference conflist trylock failed, res => %d\n", res) ;
		}
#endif
		while ( conf ) {
#endif
			//DEBUG("PROCESSING FRAMES, conference => %s, ms => %ld\n", conf->name, ( base.tv_usec / 20000 )) ;

			// acquire the conference lock
			ast_rwlock_rdlock(&conf->lock);

			//
			// check if the conference is empty and if so
			// remove it and continue to the next conference
			//

			if ( !conf->membercount )
			{
				if ( (res = ast_mutex_trylock(&conflist_lock))  )
				{
					ast_rwlock_unlock(&conf->lock);
					DEBUG("conference conflist trylock failed, res => %d,  name => %s\n", res, conf->name) ;
#ifdef	ONEMIXTHREAD	
					// get the next conference
					conf = conf->next ;
#endif
					continue ;
				}
#ifdef	APPKONFERENCE_DEBUG
				if (conf->debug_flag)
				{
					DEBUG("removing conference, count => %d, name => %s\n", conf->membercount, conf->name) ;
				}
#endif
#ifdef	ONEMIXTHREAD
				conf = remove_conf( conf ) ;

				if ( !conference_count )
					goto done42 ;
#else
				remove_conf( conf ) ;
#endif
				// We don't need to release the conf mutex, since it was destroyed anyway

				// release the conference list lock
				ast_mutex_unlock(&conflist_lock);
#ifdef	ONEMIXTHREAD
				continue ; // next conference
#else
				break ; // main loop
#endif
			}

			//
			// Start processing frames
			//

			// update the current delivery time
			conf->delivery_time = base ;

			//
			// loop through the list of members
			// ( conf->memberlist is a single-linked list )
			//

			//DEBUG("begin processing incoming audio, name => %s\n", conf->name) ;

			// reset speaker and listener count
			speaker_count = 0 ;
			listener_count = 0 ;

			// get list of conference members
			member = conf->memberlist ;

			// reset pointer lists
			spoken_frames = NULL ;
#ifdef	DTMF
			// reset dtmf source
			dtmf_source_member = NULL;
#endif
			// reset listener frame
			conf->listener_frame = NULL ;

			// loop over member list to retrieve queued frames
			while ( member )
			{
				member_process_spoken_frames(conf,member,&spoken_frames,time_diff,
							     &listener_count, &speaker_count);

				member = member->next;
			}

			//DEBUG("finished processing incoming audio, name => %s\n", conf->name) ;


			//---------------//
			// MIXING FRAMES //
			//---------------//

			// mix frames and get batch of outgoing frames
			if ( (send_frames = (spoken_frames ? mix_frames(conf, spoken_frames, speaker_count, listener_count) : NULL)) )
			{
				//DEBUG("base => %ld.%ld %d\n", base.tv_sec, base.tv_usec, ( int )( base.tv_usec / 1000 )) ;

				// accounting: if there are frames, count them as one incoming frame
				conf->stats.frames_in++ ;
			}

			//-----------------//
			// OUTGOING FRAMES //
			//-----------------//

			//
			// loop over member list to queue outgoing frames
			//
			for ( member = conf->memberlist ; member ; member = member->next )
			{
				member_process_outgoing_frames(conf, member);
			}
#ifdef	DTMF
			//------//
			// DTMF //
			//------//

			// loop over the incoming frames and send to all outgoing
			for (dtmf_source_member = conf->memberlist; dtmf_source_member ; dtmf_source_member = dtmf_source_member->next)
			{
				while ((cfr = get_incoming_dtmf_frame( dtmf_source_member )))
				{
					for (member = conf->memberlist; member ; member = member->next)
					{
						// skip members that are not ready
						if ( !member->ready_for_outgoing )
						{
							continue ;
						}

						if (member != dtmf_source_member)
						{
							// Send the latest frame
							queue_outgoing_dtmf_frame(member, cfr->fr);
						}
					}
					// Garbage collection
					delete_conf_frame(cfr);
				}
			}
#endif
			//---------//
			// CLEANUP //
			//---------//

			// clean up send frames
			while ( send_frames )
			{
				// accouting: count all frames and mixed frames
				if ( !send_frames->member )
					conf->stats.frames_out++ ;
				else
					conf->stats.frames_mixed++ ;

				// delete the frame
				send_frames = delete_conf_frame( send_frames ) ;
			}

			// release conference lock
			ast_rwlock_unlock( &conf->lock ) ;
#ifdef	ONEMIXTHREAD
			// get the next conference
			conf = conf->next ;
		}
#endif
			// !!! TESTING !!!
			// usleep( 1 ) ;
	}
#ifdef	ONEMIXTHREAD
done42:
	ast_mutex_unlock(&conflist_lock);
#endif
	// end while ( 42 == 42 )

	//
	// exit the conference thread
	//

	DEBUG("exit conference_exec\n") ;

	// exit the thread
	pthread_exit( NULL ) ;

	return ;
}

//
// manange conference functions
//

// called by app_conference.c:load_module()
void init_conference( void )
{
	ast_mutex_init( &conflist_lock ) ;

	int i;
	channel_table = malloc (CHANNEL_TABLE_SIZE * sizeof (struct channel_bucket) ) ;
	for ( i = 0; i < CHANNEL_TABLE_SIZE; i++)
		AST_LIST_HEAD_INIT (&channel_table[i]) ;
	ast_log( LOG_NOTICE, "initialized channel table, size = %d\n", CHANNEL_TABLE_SIZE ) ;

	conference_table = malloc (CONFERENCE_TABLE_SIZE * sizeof (struct conference_bucket) ) ;
	for ( i = 0; i < CONFERENCE_TABLE_SIZE; i++)
		AST_LIST_HEAD_INIT (&conference_table[i]) ;
	ast_log( LOG_NOTICE, "initialized conference table, size = %d\n", CONFERENCE_TABLE_SIZE ) ;

	argument_delimiter = ( !strcmp(PACKAGE_VERSION,"1.4") ? "|" : "," ) ;

	get_silent_frame() ;
	ast_log( LOG_NOTICE, "allocated conference silent frame\n" ) ;
}

#ifdef	CACHE_CONTROL_BLOCKS
void freeconfblocks( void )
{
	struct ast_conference *confblock;
	while ( confblocklist )
	{
		confblock = confblocklist;
		confblocklist = confblocklist->next;
		free( confblock );
	}
}
#endif

// called by app_conference.c:unload_module()
void dealloc_conference( void )
{
	int i;
	for ( i = 0; i < CHANNEL_TABLE_SIZE; i++)
		AST_LIST_HEAD_DESTROY (&channel_table[i]) ;
	free( channel_table ) ;
	ast_log( LOG_NOTICE, "destroyed channel table\n" ) ;

	for ( i = 0; i < CONFERENCE_TABLE_SIZE; i++)
		AST_LIST_HEAD_DESTROY (&conference_table[i]) ;
	free( conference_table ) ;
	ast_log( LOG_NOTICE, "destroyed conference table\n" ) ;

#ifdef	CACHE_CONTROL_BLOCKS
	freeconfblocks();
	freembrblocks();
	ast_log( LOG_NOTICE, "deallocated conference control blocks\n" ) ;
#endif

	delete_conf_frame( get_silent_frame() );
	ast_log( LOG_NOTICE, "deallocated conference silent frame\n" ) ;
}

struct ast_conference* join_conference( struct ast_conf_member* member, char* conf_name, char* max_users_flag )
{
	struct ast_conference* conf = NULL ;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);



	// look for an existing conference
	DEBUG("finding requested conference %s\n", conf_name) ;
	conf = find_conf( conf_name ) ;

	// unable to find an existing conference, try to create one
	if ( !conf )
	{
		// create a new conference
		DEBUG("creating requested conference %s\n", conf_name) ;

		// create the new conference with one member
		conf = create_conf( conf_name, member ) ;

		// return an error if create_conf() failed
		// otherwise set the member's pointer to its conference
		if ( !conf  )
			ast_log( LOG_ERROR, "unable to find or create requested conference\n" ) ;
	}
	else
	{
		//
		// existing conference found, add new member to the conference
		//
		// once we call add_member(), this thread
		// is responsible for calling delete_member()
		//
		if (!member->max_users || (member->max_users > conf->membercount)) {
			add_member( member, conf ) ;
		} else {
			pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "MAXUSERS");
			*max_users_flag = 1;
			conf = NULL;
		}
	}

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);

	return conf ;
}

// This function should be called with conflist_lock mutex being held
static struct ast_conference* find_conf( const char* name )
{
	struct ast_conference *conf ;
	struct conference_bucket *bucket = &( conference_table[hash(name) % CONFERENCE_TABLE_SIZE] ) ;

	AST_LIST_LOCK ( bucket ) ;

	AST_LIST_TRAVERSE ( bucket, conf, hash_entry )
		if (!strcmp (conf->name, name) ) {
			break ;
		}

	AST_LIST_UNLOCK ( bucket ) ;

	return conf ;
}

// This function should be called with conflist_lock held
static struct ast_conference* create_conf( char* name, struct ast_conf_member* member )
{
	//
	// allocate memory for conference
	//

	struct ast_conference *conf ;

#ifdef	CACHE_CONTROL_BLOCKS
	if ( confblocklist )
	{
		// get conference control block from the free list
		conf = confblocklist;
		confblocklist = confblocklist->next;
	}
	else
	{
#endif
		// allocate new conference control block
		if ( !(conf = malloc(sizeof(struct ast_conference))) )
		{
			ast_log( LOG_ERROR, "unable to malloc ast_conference\n" ) ;
			return NULL ;
		}
#ifdef	CACHE_CONTROL_BLOCKS
	}
#endif

	//
	// initialize conference
	//

	conf->next = NULL ;
	conf->prev = NULL ;
	conf->memberlist = NULL ;
	conf->listener_frame = NULL ;

	conf->memberlast = NULL ;

	conf->membercount = 0 ;
	conf->conference_thread = -1 ;
#ifdef	APPKONFERENCE_DEBUG
	conf->debug_flag = 0 ;
#endif
	conf->kick_flag = 0 ;

	conf->id_count = 0;
	// zero stats
	memset(	&conf->stats, 0x0, sizeof( ast_conference_stats ) ) ;

	// record start time
	conf->stats.time_entered = ast_tvnow();

	// copy name to conference
	strncpy( (char*)&(conf->name), name, sizeof(conf->name) - 1 ) ;
	strncpy( (char*)&(conf->stats.name), name, sizeof(conf->name) - 1 ) ;

	// zero volume
	conf->volume = 0;

	// initialize the conference lock
	ast_rwlock_init( &conf->lock ) ;

	// build translation paths
	conf->from_slinear_paths[ AC_SLINEAR_INDEX ] = NULL ;
#ifndef AC_USE_G722
	conf->from_slinear_paths[ AC_ULAW_INDEX ] = ast_translator_build_path( AST_FORMAT_ULAW, AST_FORMAT_SLINEAR ) ;
	conf->from_slinear_paths[ AC_ALAW_INDEX ] = ast_translator_build_path( AST_FORMAT_ALAW, AST_FORMAT_SLINEAR ) ;
	conf->from_slinear_paths[ AC_GSM_INDEX ] = ast_translator_build_path( AST_FORMAT_GSM, AST_FORMAT_SLINEAR ) ;
#ifdef	AC_USE_SPEEX
	conf->from_slinear_paths[ AC_SPEEX_INDEX ] = ast_translator_build_path( AST_FORMAT_SPEEX, AST_FORMAT_SLINEAR ) ;
#endif
#ifdef AC_USE_G729A
	conf->from_slinear_paths[ AC_G729A_INDEX ] = ast_translator_build_path( AST_FORMAT_G729A, AST_FORMAT_SLINEAR ) ;
#endif
#else
	conf->from_slinear_paths[ AC_ULAW_INDEX ] = ast_translator_build_path( AST_FORMAT_ULAW, AST_FORMAT_SLINEAR16 ) ;
	conf->from_slinear_paths[ AC_ALAW_INDEX ] = ast_translator_build_path( AST_FORMAT_ALAW, AST_FORMAT_SLINEAR16 ) ;
	conf->from_slinear_paths[ AC_GSM_INDEX ] = ast_translator_build_path( AST_FORMAT_GSM, AST_FORMAT_SLINEAR16 ) ;
#ifdef	AC_USE_SPEEX
	conf->from_slinear_paths[ AC_SPEEX_INDEX ] = ast_translator_build_path( AST_FORMAT_SPEEX, AST_FORMAT_SLINEAR16 ) ;
#endif
#ifdef AC_USE_G729A
	conf->from_slinear_paths[ AC_G729A_INDEX ] = ast_translator_build_path( AST_FORMAT_G729A, AST_FORMAT_SLINEAR16 ) ;
#endif
#ifdef AC_USE_G722
	conf->from_slinear_paths[ AC_G722_INDEX ] = ast_translator_build_path( AST_FORMAT_G722, AST_FORMAT_SLINEAR16 ) ;
#endif
#endif

	//
	// spawn thread for new conference, using conference_exec( conf )
	//
#ifdef	ONEMIXTHREAD
	if (!conflist) {
		if ( !(ast_pthread_create( &conf->conference_thread, NULL, (void*)conference_exec, NULL )) )
		{
#else
		if ( !(ast_pthread_create( &conf->conference_thread, NULL, (void*)conference_exec, conf )) )
		{
			DEBUG("started conference thread for conference, name => %s\n", conf->name) ;
#endif
			// detach the thread so it doesn't leak
			pthread_detach( conf->conference_thread ) ;
#ifdef	REALTIME
			// set scheduling if realtime
			int policy;
			struct sched_param param;

			pthread_getschedparam(conf->conference_thread, &policy, &param);

			if ( policy == SCHED_RR ) {
				++param.sched_priority;
				policy = SCHED_FIFO;
				pthread_setschedparam(conf->conference_thread, policy, &param);
			}
#endif
		}
		else
		{
			ast_log( LOG_ERROR, "unable to start conference thread for conference %s\n", conf->name ) ;

			// clean up conference
			free( conf ) ;
			return NULL ;
		}
#ifdef	ONEMIXTHREAD
	}
#endif
	// add the initial member
	add_member( member, conf ) ;

	// prepend new conference to conflist
	if (conflist)
		conflist->prev = conf;
	conf->next = conflist ;
	conflist = conf ;

	// add member to channel table
	conf->bucket = &(conference_table[hash(conf->name) % CONFERENCE_TABLE_SIZE]);

	AST_LIST_LOCK (conf->bucket ) ;
	AST_LIST_INSERT_HEAD (conf->bucket, conf, hash_entry) ;
	AST_LIST_UNLOCK (conf->bucket ) ;

	DEBUG("added %s to the channel table, bucket => %ld\n", conf->name, conf->bucket - conference_table) ;

	DEBUG("added %s to conference list\n", name) ;

	// count new conference
	++conference_count ;

	return conf ;
}

//This function should be called with conflist_lock and conf->lock held
struct ast_conference *remove_conf( struct ast_conference *conf )
{

	struct ast_conference *conf_temp ;

	DEBUG("removing conference %s\n", conf->name) ;

	//
	// do some frame clean up
	//

	int c;
	for ( c = 0 ; c < AC_SUPPORTED_FORMATS ; ++c )
	{
		// free the translation paths
		if ( conf->from_slinear_paths[ c ] )
		{
			ast_translator_free_path( conf->from_slinear_paths[ c ] ) ;
			conf->from_slinear_paths[ c ] = NULL ;
		}
	}
#ifdef	APP_KONFERENCE_DEBUG
	if (conf->debug_flag)
	{
		// report accounting information

		// calculate time in conference
		// total time converted to seconds
		long tt = ast_tvdiff_ms(ast_tvnow(),
				conf->stats.time_entered) / 1000;

		DEBUG("conference accounting, conference => %s, fi => %ld, fo => %ld, fm => %ld, tt => %ld\n", conf->name, conf->stats.frames_in, conf->stats.frames_out, conf->stats.frames_mixed, tt) ;

	}
#endif

	AST_LIST_LOCK (conf->bucket ) ;
	AST_LIST_REMOVE (conf->bucket, conf, hash_entry) ;
	AST_LIST_UNLOCK (conf->bucket ) ;

	DEBUG("removed %s from the conference table, bucket => %ld\n", conf->name, conf->bucket - conference_table) ;

	// unlock and destroy read/write lock
	ast_rwlock_unlock( &conf->lock ) ;
	ast_rwlock_destroy( &conf->lock ) ;

	conf_temp = conf->next ;

	if ( conf->prev )
		conf->prev->next = conf->next ;

	if ( conf->next )
		conf->next->prev = conf->prev ;

	if ( conf == conflist )
		conflist = conf_temp ;
#ifdef	CACHE_CONTROL_BLOCKS
	// put the conference control block on the free list
	conf->next = confblocklist;
	confblocklist = conf;
#else
	free( conf ) ;	
#endif
	// update conference count
	--conference_count ;

	return conf_temp ;

}
int end_conference(const char *name, int hangup )
{
	struct ast_conference *conf;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);

	conf = find_conf(name);
	if ( !conf )
	{
		DEBUG("could not find conference %s\n", name) ;

		// release the conference list lock
		ast_mutex_unlock(&conflist_lock);

		return -1 ;
	}

	// acquire the conference lock
	ast_rwlock_rdlock( &conf->lock ) ;

	// get list of conference members
	struct ast_conf_member* member = conf->memberlist ;

	// loop over member list and request hangup
	while ( member )
	{
		// acquire member mutex and request hangup
		// or just kick
		ast_mutex_lock( &member->lock ) ;
		if (hangup)
			ast_softhangup( member->chan, 1 ) ;
		else
			member->kick_flag = 1;
		ast_mutex_unlock( &member->lock ) ;

		// go on to the next member
		// ( we have the conf lock, so we know this is okay )
		member = member->next ;
	}

	// release the conference lock
	ast_rwlock_unlock( &conf->lock ) ;

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);

	return 0 ;
}

//
// member-related functions
//

// This function should be called with conflist_lock held
static void add_member( struct ast_conf_member *member, struct ast_conference *conf )
{
#ifdef	APP_KONFERENCE_DEBUG
	if ( !conf )
	{
		ast_log( LOG_ERROR, "unable to add member to NULL conference\n" ) ;
		return ;
	}
#endif
	// acquire the conference lock
	ast_rwlock_wrlock( &conf->lock ) ;

	//
	// if spying, setup spyer/spyee
	//
	if ( member->spyee_channel_name )
	{
		struct ast_conf_member *spyee = find_member(member->spyee_channel_name, 0);
		if ( spyee && !spyee->spy_partner && spyee->conf == conf )
		{
			spyee->spy_partner = member;
			member->spy_partner = spyee;
			member->spyer = 1;
		}
	}

	// update conference stats
	conf->membercount++;

	if ( member->hold_flag == 1 )
	{
		if  ( conf->membercount == 1 )
		{
			ast_mutex_lock( &member->lock ) ;
			member->moh_flag = 1 ;
			ast_mutex_unlock( &member->lock ) ;
		}
		else if ( conf->membercount == 2 && conf->memberlist->hold_flag == 1)
		{
			ast_mutex_lock( &conf->memberlist->lock ) ;
			conf->memberlist->moh_flag = 0 ;
			conf->memberlist->ready_for_outgoing = 1;
			conf->memberlist->moh_stop = 1;
			ast_mutex_unlock( &conf->memberlist->lock ) ;
		}
	}

	if (member->ismoderator)
		conf->stats.moderators++;

	member->id = ( !conf->memberlast ? 1 : conf->memberlast->id + 1 ) ;

	if ( !conf->memberlist )
		conf->memberlist = conf->memberlast = member ;
	else {
		member->prev = conf->memberlast ; // dbl links
		conf->memberlast->next = member ;
		conf->memberlast = member ;
	}
	member->conf = conf ;

	// release the conference lock
	ast_rwlock_unlock( &conf->lock ) ;

	DEBUG("added member to conference %s\n", conf->name) ;

	return ;
}

void remove_member( struct ast_conf_member* member, struct ast_conference* conf, char* conf_name )
{
	int membercount ;
	short moderators ;
	long tt ;
#ifdef	APP_KONFERENCE_DEBUG
	// check for member
	if ( !member )
	{
		ast_log( LOG_WARNING, "unable to remove NULL member\n" ) ;
		return ;
	}

	// check for conference
	if ( !conf )
	{
		ast_log( LOG_WARNING, "unable to remove member from NULL conference\n" ) ;
		return  ;
	}
#endif
	//
	// loop through the member list looking
	// for the requested member
	//

	ast_rwlock_wrlock( &conf->lock );

	if ( member->ismoderator && member->kick_conferees && conf->stats.moderators == 1 )
		conf->kick_flag = 1 ;

	struct ast_conf_member *member_temp = member->prev ;

			// calculate time in conference (in seconds)
			tt = ast_tvdiff_ms(ast_tvnow(),
					member->time_entered) / 1000;
#ifdef	APP_KONFERENCE_DEBUG
			if (conf->debug_flag)
			{
				//
				// log some accounting information
				//

				DEBUG("member accounting, channel => %s, te => %ld, fi => %ld, fid => %ld, fo => %ld, fod => %ld, tt => %ld\n", member->chan->name, member->time_entered.tv_sec, member->frames_in, member->frames_in_dropped, member->frames_out, member->frames_out_dropped, tt) ;
			}
#endif
			//
			// if this is the first member in the linked-list,
			// skip over the first member in the list, else
			//
			// point the previous 'next' to the current 'next',
			// thus skipping the current member in the list
			//
			if ( !member_temp )
				conf->memberlist = member->next ;
			else
				member_temp->next = member->next ;

			if(member->next) member->next->prev =  member_temp ; // dbl links

			if ( conf->memberlast == member )
				conf->memberlast = ( !member_temp ? NULL : member_temp );

			// update conference stats
			membercount = --conf->membercount;

			if ( member->hold_flag == 1 && conf->membercount == 1 && conf->memberlist->hold_flag == 1 )
			{
					ast_mutex_lock( &conf->memberlist->lock ) ;
					conf->memberlist->moh_flag = 1 ;
					ast_mutex_unlock( &conf->memberlist->lock ) ;
			}

			// update moderator count
			moderators = (!member->ismoderator ? conf->stats.moderators : --conf->stats.moderators );
	//
	// if spying sever connection to spyee
	//
	if ( member->spy_partner )
	{
		member->spy_partner->spy_partner = NULL;
		member->spy_partner->spyer = 0;
		member->spy_partner = NULL;
		member->spyer = 0;
	}

	ast_rwlock_unlock( &conf->lock );

	DEBUG("removed member from conference %s, remaining members => %d\n", conf_name, membercount) ;

	// remove member from channel table
	if ( member->bucket )
	{
		AST_LIST_LOCK (member->bucket ) ;
		AST_LIST_REMOVE (member->bucket, member, hash_entry) ;
		AST_LIST_UNLOCK (member->bucket ) ;
		DEBUG("removed %s from the channel table, bucket => %ld\n", member->chan->name, member->bucket - channel_table) ;
	}

	// output to manager...
	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceLeave",
		"ConferenceName: %s\r\n"
		"Type:  %s\r\n"
		"UniqueID: %s\r\n"
		"Member: %d\r\n"
		"Flags: %s\r\n"
		"Channel: %s\r\n"
		"CallerID: %s\r\n"
		"CallerIDName: %s\r\n"
		"Duration: %ld\r\n"
		"Moderators: %d\r\n"
		"Count: %d\r\n",
		conf_name,
		member->type,
		member->chan->uniqueid,
		member->id,
		member->flags,
		member->chan->name,
#if	ASTERISK == 14 || ASTERISK == 16
		member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
		member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
#else
		member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
		member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
#endif
		tt,
		moderators,
		membercount
	) ;

	// delete the member
	delete_member( member ) ;

}

#ifdef	APP_KONFERENCE_DEBUG
//
// returns: -1 => error, 0 => debugging off, 1 => debugging on
// state: on => 1, off => 0, toggle => -1
//
int set_conference_debugging( const char* name, int state )
{
	if ( !name )
		return -1 ;

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;
	int new_state = -1 ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), name ) )
		{
			// lock conference
			// ast_mutex_lock( &(conf->lock) ) ;

			// toggle or set the state
			if ( state == -1 )
				conf->debug_flag = ( !conf->debug_flag  ? 1 : 0 ) ;
			else
				conf->debug_flag = ( !state ? 0 : 1) ;

			new_state = conf->debug_flag ;

			// unlock conference
			// ast_mutex_unlock( &(conf->lock) ) ;

			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return new_state ;
}
#endif

int get_conference_count( void )
{
	return conference_count ;
}

int show_conference_stats ( int fd )
{
	int duration;
	char duration_str[10];

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized.\n") ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	ast_cli( fd, "%-20.20s %-20.20s %-20.20s %-20.20s\n", "Name", "Members", "Volume", "Duration" ) ;

	// loop through conf list
	while ( conf )
	{
		duration = (int)(ast_tvdiff_ms(ast_tvnow(),conf->stats.time_entered) / 1000);
		snprintf(duration_str, 10, "%02d:%02d:%02d",  duration / 3600, (duration % 3600) / 60, duration % 60);
		ast_cli( fd, "%-20.20s %-20d %-20d %-20.20s\n", conf->name, conf->membercount, conf->volume, duration_str ) ;
		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return 1 ;
}

int show_conference_list ( int fd, const char *name )
{
	struct ast_conf_member *member;
	char volume_str[10];
	char spy_str[10];
	int duration;
	char duration_str[10];

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", name) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), name ) )
		{
			// acquire conference lock
			ast_rwlock_rdlock(&conf->lock);

			// ast_cli(fd, "Chat mode is %s\n", conf->chat_mode_on ? "ON" : "OFF");
			ast_cli( fd, "%-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80.20s\n", "User #", "Flags", "Audio", "Volume", "Duration", "Spy", "Channel");
			// do the biz
			member = conf->memberlist ;
			while ( member )
			{
				snprintf(volume_str, 10, "%d:%d", member->talk_volume, member->listen_volume);
				if ( member->spyee_channel_name )
					snprintf(spy_str, 10, "%d", member->spy_partner->id);
				else
					strcpy(spy_str , "*");
				duration = (int)(ast_tvdiff_ms(ast_tvnow(),member->time_entered) / 1000);
				snprintf(duration_str, 10, "%02d:%02d:%02d",  duration / 3600, (duration % 3600) / 60, duration % 60);
				ast_cli( fd, "%-20d %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80s\n",
				member->id, member->flags, !member->mute_audio ? "Unmuted" : "Muted", volume_str, duration_str , spy_str, member->chan->name);
				member = member->next;
			}

			// release conference lock
			ast_rwlock_unlock(&conf->lock);

			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return 1 ;
}
#ifdef	MANAGER_COMMANDS
/* Dump list of conference info */
int manager_conference_list( struct mansession *s, const struct message *m )
{
	const char *id = astman_get_header(m,"ActionID");
	const char *conffilter = astman_get_header(m,"Conference");
	char idText[256] = "";
	struct ast_conf_member *member;

	astman_send_ack(s, m, "Conference list will follow");

  // no conferences exist
	if ( !conflist )
		DEBUG("conflist has not yet been initialized, name => %s\n", conffilter) ;

	if (!ast_strlen_zero(id)) {
		snprintf(idText,256,"ActionID: %s\r\n",id);
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), conffilter ) )
		{
			// do the biz
			member = conf->memberlist ;
			while (member )
			  {
					astman_append(s, "Event: ConferenceEntry\r\n"
						"ConferenceName: %s\r\n"
						"Member: %d\r\n"
						"Channel: %s\r\n"
						"CallerID: %s\r\n"
						"CallerIDName: %s\r\n"
						"Muted: %s\r\n"
						"%s"
						"\r\n",
						conf->name,
						member->id,
						member->chan->name,
						member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
						member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
						member->mute_audio ? "YES" : "NO",
						idText);
			    member = member->next;
			  }
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	astman_append(s,
		"Event: ConferenceListComplete\r\n"
		"%s"
		"\r\n",idText);

	return RESULT_SUCCESS;
}

//
// E.BUU - Manager conference end. Additional option to just kick everybody out
// without hangin up channels
//
int manager_conference_end(struct mansession *s, const struct message *m)
{
	const char *confname = astman_get_header(m,"Conference");
	int hangup = 1;

	const char * h =  astman_get_header(m, "Hangup");
	if (h)
	{
		hangup = atoi(h);
	}

	ast_log( LOG_NOTICE, "Terminating conference %s on manager's request. Hangup: %s.\n", confname, hangup?"YES":"NO" );
        if ( end_conference( confname, hangup ) )
        {
		ast_log( LOG_ERROR, "manager end conf: unable to terminate conference %s.\n", confname );
		astman_send_error(s, m, "Failed to terminate\r\n");
		return RESULT_FAILURE;
	}

	astman_send_ack(s, m, "Conference terminated");
	return RESULT_SUCCESS;
}
#endif
int kick_member (  const char* confname, int user_id)
{
	struct ast_conf_member *member;
	int res = 0;

	// no conferences exist
	if ( !conflist )
	{
		DEBUG( "conflist has not yet been initialized, name => %s\n", confname ) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), confname ) )
		{
		        // do the biz
			ast_rwlock_rdlock( &conf->lock ) ;
		        member = conf->memberlist ;
			while (member )
			  {
			    if (member->id == user_id)
			      {
				      ast_mutex_lock( &member->lock ) ;
				      member->kick_flag = 1;
				      //ast_soft_hangup(member->chan);
				      ast_mutex_unlock( &member->lock ) ;

				      res = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock( &conf->lock ) ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return res ;
}

int kick_all ( void )
{
  struct ast_conf_member *member;
  int res = 0;

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized\n" ) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		// do the biz
		ast_rwlock_rdlock( &conf->lock ) ;
		member = conf->memberlist ;
		while (member )
		{
			ast_mutex_lock( &member->lock ) ;
			member->kick_flag = 1;
			ast_mutex_unlock( &member->lock ) ;
			member = member->next;
		}
		ast_rwlock_unlock( &conf->lock ) ;
		break ;

	conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return res ;
}

int mute_member (  const char* confname, int user_id)
{
  struct ast_conf_member *member;
  int res = 0;

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", confname) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), confname ) )
		{
		        // do the biz
			ast_rwlock_rdlock( &conf->lock ) ;
		        member = conf->memberlist ;
			while (member )
			  {
			    if (member->id == user_id)
			      {
				      ast_mutex_lock( &member->lock ) ;
				      member->mute_audio = 1;
				      ast_mutex_unlock( &member->lock ) ;
					manager_event(
						EVENT_FLAG_CONF,
						"ConferenceMemberMute",
						"Channel: %s\r\n",
						member->chan->name
					) ;
				      res = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock( &conf->lock ) ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return res ;
}

int mute_conference (  const char* confname)
{
  struct ast_conf_member *member;
  int res = 0;

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", confname) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), confname ) )
		{
		        // do the biz
			ast_rwlock_rdlock( &conf->lock ) ;
		        member = conf->memberlist ;
			while (member )
			  {
			    if ( !member->ismoderator )
			      {
				      ast_mutex_lock( &member->lock ) ;
				      member->mute_audio = 1;
				      ast_mutex_unlock( &member->lock ) ;
				      res = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock( &conf->lock ) ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceMute",
		"ConferenceName: %s\r\n",
		confname
	) ;

	return res ;
}

int unmute_member (  const char* confname, int user_id)
{
  struct ast_conf_member *member;
  int res = 0;

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", confname) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), confname ) )
		{
		        // do the biz
			ast_rwlock_rdlock( &conf->lock ) ;
		        member = conf->memberlist ;
			while (member )
			  {
			    if (member->id == user_id)
			      {
				      ast_mutex_lock( &member->lock ) ;
				      member->mute_audio = 0;
				      ast_mutex_unlock( &member->lock ) ;
					manager_event(
						EVENT_FLAG_CONF,
						"ConferenceMemberUnmute",
						"Channel: %s\r\n",
						member->chan->name
					) ;
				      res = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock( &conf->lock ) ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return res ;
}

int unmute_conference (  const char* confname)
{
  struct ast_conf_member *member;
  int res = 0;

        // no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", confname) ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), confname ) )
		{
		        // do the biz
			ast_rwlock_rdlock( &conf->lock ) ;
		        member = conf->memberlist ;
			while (member )
			  {
			    if ( !member->ismoderator )
			      {
				      ast_mutex_lock( &member->lock ) ;
				      member->mute_audio = 0;
				      ast_mutex_unlock( &member->lock ) ;
				      res = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock( &conf->lock ) ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceUnmute",
		"ConferenceName: %s\r\n",
		confname
	) ;

	return res ;
}

int get_conference_stats( ast_conference_stats* stats, int requested )
{
	// no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialize\n") ;
		return 0 ;
	}

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	// compare the number of requested to the number of available conferences
	requested = ( get_conference_count() < requested ? get_conference_count() : requested)  ;

	//
	// loop through conf list
	//

	struct ast_conference* conf = conflist ;
	int count = 0 ;

	while ( count <= requested && conf )
	{
		// copy stats struct to array
		stats[ count ] = conf->stats ;

		conf = conf->next ;
		++count ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return count ;
}

int get_conference_stats_by_name( ast_conference_stats* stats, const char* name )
{
	// no conferences exist
	if ( !conflist )
	{
		DEBUG("conflist has not yet been initialized, name => %s\n", name) ;
		return 0 ;
	}

	// make sure stats is null
	stats = NULL ;

	// acquire mutex
	ast_mutex_lock( &conflist_lock ) ;

	struct ast_conference *conf = conflist ;

	// loop through conf list
	while ( conf )
	{
		if ( !strcasecmp( (const char*)&(conf->name), name ) )
		{
			// copy stats for found conference
			*stats = conf->stats ;
			break ;
		}

		conf = conf->next ;
	}

	// release mutex
	ast_mutex_unlock( &conflist_lock ) ;

	return (!stats ? 0 : 1) ;
}

struct ast_conf_member *find_member( const char *chan, const char lock )
{
	struct ast_conf_member *member ;
	struct channel_bucket *bucket = &( channel_table[hash(chan) % CHANNEL_TABLE_SIZE] ) ;

	AST_LIST_LOCK ( bucket ) ;

	AST_LIST_TRAVERSE ( bucket, member, hash_entry )
		if (!strcmp (member->chan->name, chan) ) {
			if (lock)
			{
				ast_mutex_lock (&member->lock) ;
				member->use_count++ ;
			}
			break ;
		}

	AST_LIST_UNLOCK ( bucket ) ;

	return member ;
}

#if	ASTERISK == 14 || ASTERISK == 16
int play_sound_channel(int fd, const char *channel, char **file, int mute, int tone, int n)
#else
int play_sound_channel(int fd, const char *channel, const char * const *file, int mute, int tone, int n)
#endif
{
	struct ast_conf_member *member;
	struct ast_conf_soundq *newsound;
	struct ast_conf_soundq **q;

	ast_cli(fd, "Playing sound %s to member %s %s\n",
		      *file, channel, mute ? "with mute" : "");

	member = find_member(channel, 1);
	if( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	} else if (!member->norecv_audio && !member->moh_flag
			&& (!tone || !member->soundq))
	{
		while ( n-- > 0 ) {
			if( !(newsound = ast_calloc(1, sizeof(struct ast_conf_soundq))))
				break ;

			ast_copy_string(newsound->name, *file, sizeof(newsound->name));

			// append sound to the end of the list.
			for ( q=&member->soundq; *q; q = &((*q)->next) ) ;
			*q = newsound;

			file++;
		}

		member->muted = mute;

	}
	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1 ;
}

int stop_sound_channel(int fd, const char *channel)
{
	struct ast_conf_member *member;
	struct ast_conf_soundq *sound;
	struct ast_conf_soundq *next;

	ast_cli( fd, "Stopping sounds to member %s\n", channel);

	member = find_member(channel, 1);
	if ( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	}

	// clear all sounds
	sound = member->soundq;

	while ( sound )
	{
		next = sound->next;
		sound->stopped = 1;
		sound = next;
	}

		member->muted = 0;

	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1;
}

int start_moh_channel(int fd, const char *channel)
{
	struct ast_conf_member *member;

	ast_cli( fd, "Starting moh to member %s\n", channel);

	member = find_member(channel, 1);
	if ( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	} else if (!member->norecv_audio && !member->moh_flag)
	{
		member->moh_flag = member->muted = 1;
	}

	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1;
}

int stop_moh_channel(int fd, const char *channel)
{
	struct ast_conf_member *member;

	ast_cli( fd, "Stopping moh to member %s\n", channel);

	member = find_member(channel, 1);
	if ( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	} else if (!member->norecv_audio && member->moh_flag)
	{
		member->moh_stop = 1;

		member->moh_flag = member->muted = 0;
		member->ready_for_outgoing = 1;
	}

	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1;
}

int talk_volume_channel(int fd, const char *channel, int up)
{
	struct ast_conf_member *member;

	ast_cli( fd, "Adjusting talk volume level %s for member %s\n", up ? "up" : "down", channel);

	member = find_member(channel, 1);
	if ( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	}

	up ? member->talk_volume++ : member->talk_volume--;

	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1;
}

int listen_volume_channel(int fd, const char *channel, int up)
{
	struct ast_conf_member *member;

	ast_cli( fd, "Adjusting listen volume level %s for member %s\n", up ? "up" : "down", channel);

	member = find_member(channel, 1);
	if ( !member )
	{
		ast_cli(fd, "Member %s not found\n", channel);
		return 0;
	}

	up ? member->listen_volume++ : member->listen_volume--;

	if ( !--member->use_count && member->delete_flag )
		ast_cond_signal ( &member->delete_var ) ;
	ast_mutex_unlock(&member->lock);

	return 1;
}

int volume(int fd, const char *conference, int up)
{
	struct ast_conference *conf;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);

	conf = find_conf(conference);
	if ( conf == NULL )
	{
		// release the conference list lock
		ast_mutex_unlock(&conflist_lock);

		ast_cli( fd, "Conference %s not found\n", conference ) ;
		return 0;
	}

	// acquire the conference lock
	ast_rwlock_wrlock( &conf->lock ) ;

	// adjust volume
	up ? conf->volume++ : conf->volume--;

	// release the conference lock
	ast_rwlock_unlock( &conf->lock ) ;

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);

	return 1;
}

int hash(const char *name)
{
	int i = 0, h = 0, g;
	while (name[i])
	{
		h = (h << 4) + name[i++];
		if ( (g = h & 0xF0000000) )
			h ^= g >> 24;
		h &= ~g;
	}
	//DEBUG("Hashed name: %s to 0x%08x\n", name, h) ;
	return h;
}

#if	ASTERISK == 14
int count_exec( struct ast_channel* chan, void* data )
#else
int count_exec( struct ast_channel* chan, const char* data )
#endif
{
	int res = 0;
	struct ast_conference *conf;
	int count;
	char *localdata;
	char val[80] = "0"; 
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(varname);
	);
	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "ConferenceCount requires an argument (conference number)\n");
		return -1;
	}

	if (!(localdata = ast_strdupa(data))) {
		return -1;
	}

	AST_STANDARD_APP_ARGS(args, localdata);
	
	ast_mutex_lock(&conflist_lock) ;

	conf = find_conf(args.confno);

	if (conf) {
		count = conf->membercount;
	} else
		count = 0;

	ast_mutex_unlock(&conflist_lock) ;

	if (!ast_strlen_zero(args.varname)){
		snprintf(val, sizeof(val), "%d",count);
		pbx_builtin_setvar_helper(chan, args.varname, val);
	} else {
		if (chan->_state != AST_STATE_UP)
			ast_answer(chan);
		res = ast_say_number(chan, count, "", chan->language, (char *) NULL);
	}
	return res;
}

// increment a timeval by ms milliseconds
void add_milliseconds(struct timeval* tv, long ms)
{
	// add the microseconds to the microseconds field
	tv->tv_usec += ( ms * 1000 ) ;

	// calculate the number of seconds to increment
	long s = ( tv->tv_usec / 1000000 ) ;

	// adjust the microsends field
	if ( s > 0 ) tv->tv_usec -= ( s * 1000000 ) ;

	// increment the seconds field
	tv->tv_sec += s ;
}
