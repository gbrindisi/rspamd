
=head1 NAME

Mail::Rspamd::Client - Client for rspamd Protocol


=head1 SYNOPSIS

  my $client = new Mail::Rspamd::Client({port => 11333,
                                               host => 'localhost',
                                               ip => '127.0.0.1'});

  if ($client->ping()) {
    print "Ping is ok\n";
  }

  my $result = $client->check($testmsg);

  if ($result->{'default'}->{isspam} eq 'True') {
    do something with spam message here
  }

=head1 DESCRIPTION

Mail::Rspamd::Client is a module that provides a perl implementation for
the spamd protocol.

=cut

package Mail::Rspamd::Client;

use IO::Socket;
use IO::Select;

my $EOL = "\015\012";
my $BLANK = $EOL x 2;
my $PROTOVERSION = 'RSPAMC/1.0';

=head1 PUBLIC METHODS

=head2 new

public class (Mail::Rspamd::Client) new (\% $args)

Description:
This method creates a new Mail::Rspamd::Client object.

=cut

sub new {
  my ($class, $args) = @_;

  $class = ref($class) || $class;

  my $self = {};

  # with a sockets_path set then it makes no sense to set host and port
  if ($args->{socketpath}) {
    $self->{socketpath} = $args->{socketpath};
  }
  else {
    $self->{hosts} = $args->{hosts};
    $self->{alive_hosts} = $self->{hosts};
  }

  if ($args->{username}) {
    $self->{username} = $args->{username};
  }
  if ($args->{ip}) {
    $self->{ip} = $args->{ip};
  }
  if ($args->{from}) {
    $self->{from} = $args->{from};
  }
  if ($args->{subject}) {
    $self->{subject} = $args->{subject};
  }
  if ($args->{rcpt}) {
    $self->{rcpt} = $args->{rcpt};
  }
  if ($args->{timeout}) {
    $self->{timeout} = $args->{timeout};
  }
  else {
    $self->{timeout} = 5;
  }
  if ($args->{password}) {
    $self->{password} = $args->{password};
  }

  bless($self, $class);

  $self;
}

=head2 check

public instance (\%) check (String $msg)

Description:
This method makes a call to the spamd server and depending on the value of
C<$is_check_p> either calls PROCESS or CHECK.

The return value is a hash reference containing metrics indexed by name. Each metric
is hash that contains data:

isspam

score

threshold

symbols - array of symbols

=cut

sub check {
  my ($self, $msg) = @_;

  my %metrics;

  my $command = 'SYMBOLS';

  $self->_clear_errors();

  my $remote = $self->_create_connection();

  return 0 unless ($remote);

  my $msgsize = length($msg.$EOL);

  local $SIG{PIPE} = 'IGNORE';

  if (!(print $remote "$command $PROTOVERSION$EOL")) {
    $self->_mark_dead($remote);
    return 0;
  }
  print $remote "Content-length: $msgsize$EOL";
  print $remote "User: $self->{username}$EOL" if ($self->{username});
  print $remote "From: $self->{from}$EOL" if ($self->{from});
  print $remote "IP: $self->{ip}$EOL" if ($self->{ip});
  print $remote "Subject: $self->{subject}$EOL" if ($self->{subject});
  if (ref $self->{rcpt} eq "ARRAY") {
    foreach ($self->{rcpt}) {
      print $remote "Rcpt: $_ $EOL";
    }
  }
  print $remote "$EOL";
  print $remote $msg;
  print $remote "$EOL";
  
  return undef unless $self->_get_io_readiness($remote, 0);

  my $line = <$remote>;
  return undef unless (defined $line);

  my ($version, $resp_code, $resp_msg) = $self->_parse_response_line($line);

  $self->{resp_code} = $resp_code;
  $self->{resp_msg} = $resp_msg;

  return undef unless ($resp_code == 0);

  my $cur_metric;

  while ($self->_get_io_readiness($remote, 0) && ($line = <$remote>)) {
    if ($line =~ m!Metric: (\S+); (\S+); (\S+) / (\S+)!) {
      $metrics{$1} = {
        isspam => $2,
        score => $3 + 0,
        threshold => $4 + 0,
        symbols => [],
      };
      $cur_metric = $1;
    }
    elsif ($line =~ /^Symbol: (\S+)/ && $cur_metric) {
      my $symref = $metrics{$cur_metric}->{'symbols'};
      push(@$symref, $1);
    }
    elsif ($line =~ /^${EOL}$/) {
      last;
    }
  }

  close $remote;

  return \%metrics;
}

sub _auth {
  my ($self, $sock) = @_;

  local $SIG{PIPE} = 'IGNORE';

  if (!(print $sock "PASSWORD $self->{password}$EOL")) {
    $self->_mark_dead($remote);
    return 0;
  }

  return 0 unless $self->_get_io_readiness($sock, 0);

  if (defined (my $reply = <$sock>)) {
    my $end = <$sock>;
    if ($reply =~ /^password accepted/) {
      return 1;
    }
  }

  return 0;
  
}

=head2 learn

public instance (\%) check (String $msg, String $statfile, Boolean in_class)

Description:
This method makes a call to the spamd learning a statfile with message.

=cut

sub learn {
  my ($self, $msg, $statfile, $in_class) = @_;

  my %metrics;

  my $command = 'LEARN';

  $self->_clear_errors();

  my $remote = $self->_create_connection();

  return 0 unless ($remote);

  return 0 unless $self->_auth($remote);

  my $msgsize = length($msg.$EOL);

  local $SIG{PIPE} = 'IGNORE';

  if (!(print $remote "$command $statfile $msgsize$EOL")) {
    $self->_mark_dead($remote);
    return 0;
  }

  print $remote $msg;
  print $remote "$EOL";
  
  return undef unless $self->_get_io_readiness($remote, 0);
  if (defined (my $reply = <$sock>)) {
    if ($reply =~ /^learn ok/) {
      close $remote;
      return 1;
    }
  }

  close $remote;
  return 0;
}

=head2 ping

public instance (Boolean) ping ()

Description:
This method performs a server ping and returns 0 or 1 depending on
if the server responded correctly.

=cut

sub ping {
  my ($self) = @_;

  my $remote = $self->_create_connection();

  return 0 unless ($remote);
  local $SIG{PIPE} = 'IGNORE';

  if (!(print $remote "PING $PROTOVERSION$EOL")) {
    $self->_mark_dead($remote);
    return 0;
  }
  print $remote "$EOL";

  return undef unless $self->_get_io_readiness($remote, 0);
  my $line = <$remote>;
  close $remote;
  return undef unless (defined $line);

  my ($version, $resp_code, $resp_msg) = $self->_parse_response_line($line);
  return 0 unless ($resp_msg eq 'PONG');

  return 1;
}

=head1 PRIVATE METHODS

=head2 _create_connection

private instance (IO::Socket) _create_connection ()

Description:
This method sets up a proper IO::Socket connection based on the arguments
used when greating the client object.

On failure, it sets an internal error code and returns undef.

=cut

sub _create_connection {
  my ($self) = @_;

  my $remote;
  my $tries = 0;

  if ($self->{socketpath}) {
    $remote = IO::Socket::UNIX->new( Peer => $self->{socketpath},
				     Type => SOCK_STREAM,
             Blocking  => 0,
				   );
    # Get write readiness
    if ($self->_get_io_readiness($remote, 1) == 0) {
      print "Connection timed out: $!\n";
      return undef;
    }
  }
  else {
    my $server;

    do {
      $server = $self->_select_server();
      $tries ++;

      $remote = IO::Socket::INET->new( Proto     => "tcp",
                PeerAddr  => $server->{host},
                PeerPort  => $server->{port},
                Blocking  => 0,
              );
      # Get write readiness
      if ($self->_get_io_readiness($remote, 1) != 0) {
        return $remote;
      }
      else {
        next;
      }
    } while ($tries < 5);

    return undef unless $server;
  }
  unless ($remote) {
    print "Failed to create connection to spamd daemon: $!\n";
    return undef;
  }
  $remote;
}

sub _revive_dead {
  my ($self) = @_;

  my $now = time();
  foreach my $s ($self->{dead_hosts}) {
    # revive after minute of downtime
    if ($s->{dead} == 1 && $now - $s->{t} > 60) {
      $s->{dead} = 0;
      push(@{$self->{alive_hosts}}, $s->{host});
    }
  }

  1;
}

sub _select_server {
  my($self) = @_;
    
  $self->_revive_dead();
  my $alive_num = scalar(@{$self->{alive_hosts}});
  if (!$alive_num) {
    $self->{alive_hosts} = $self->{hosts};
    $self->{dead_hosts} = ();
    $alive_num = scalar($self->{alive_hosts});
  }
  
  my $selected = $self->{alive_hosts}[int(rand($alive_num))];
  if ($selected =~ /^(\S+):(\d+)$/) {
    my $server = {
        host => $1,
        port => $2,
    };
    return $server;
  }

  undef;
}


sub _mark_dead {
  my ($self, $server) = @_;
  
  my $now = time();
  $self->{dead_hosts}->{$server} = {
    host => $server,
    dead => 1,
    t => $now,
  };
  for (my $i = 0; $i < scalar (@{$self->{alive_hosts}}); $i ++) {
    if ($self->{alive_hosts} == $server) {
      splice(@{$self->{alive_hosts}}, $i, 1);
      last;
    }
  }
}

sub _get_io_readiness {
  my ($self, $sock, $is_write) = @_;
  my $s = IO::Select->new();
  $s->add($sock);

  if ($is_write) {
    @ready = $s->can_write($self->{timeout});
  }
  else {
    @ready = $s->can_read($self->{timeout});
  }
  

  scalar(@ready);
}

=head2 _parse_response_line

private instance (@) _parse_response_line (String $line)

Description:
This method parses the initial response line/header from the server
and returns its parts.

We have this as a seperate method in case we ever decide to get fancy
with the response line.

=cut

sub _parse_response_line {
  my ($self, $line) = @_;

  $line =~ s/\r?\n$//;
  return split(/\s+/, $line, 3);
}

=head2 _clear_errors

private instance () _clear_errors ()

Description:
This method clears out any current errors.

=cut

sub _clear_errors {
  my ($self) = @_;

  $self->{resp_code} = undef;
  $self->{resp_msg} = undef;
}

1;


