<?php
/**
 * @file
 * The client application DBus authentication class.
 */

class mediamosa_app_dbus {
  // ------------------------------------------------------------------- Consts.
  // Client Application authentication settings.
  const APP_AUTH_CONTEXT = 'vpx';
  const APP_AUTH_SECRET_PHRASE = '41d0c72bd73afaa2c207064d81d5a3d9';
  const APP_AUTH_SERVER_GUID = 'server_guid';

  // ---------------------------------------------------------------- Functions.
  /**
   * Get the shared key for the (active) application.
   *
   * @param string $name
   *   The name of the client app.
   *
   * @return string
   *   The shared key (password).
   */
  public static function get_shared_key($name) {
    return mediamosa_db::db_select(mediamosa_app_db::TABLE_NAME, 'app')
      ->fields('app', array(mediamosa_app_db::SHARED_KEY))
      ->condition(mediamosa_app_db::APP_NAME, $name)
      ->condition(mediamosa_app_db::ACTIVE, mediamosa_app_db::ACTIVE_TRUE)
      ->execute()
      ->fetchField();
  }

  /**
   * Get the client app.
   *
   * @todo: remove and use mediamos_app::get_by_name function directly.
   *
   * @param string $name
   *   The name of the app to get.
   *
   * @return array
   *   The client app or FALSE.
   */
  public static function get_app($name) {
    return mediamosa_app::get_by_name($name);
  }

  /**
   * Get the secret random phrase.
   *
   * @return string
   *   The secret phrase.
   */
  protected static function get_secret_phrase() {
    return self::APP_AUTH_SECRET_PHRASE;
  }

  /**
   * A (cookie) ID authentication method.
   *
   * @param string $command
   *   The DBus command, either 'AUTH' or 'DATA'.
   * @param string $param
   *   The DBus command parameter.
   *
   * @return string | boolean
   *   The DBus response (string) or boolean for failure status. Will return
   *   FALSE when authentication failed, TRUE when success.
   */
  public static function dbus_cookie_sha1($command, $param) {

    switch ($command) {
      case 'AUTH':
        if (!mediamosa_app::is_app_active($param)) {
          session_destroy();
          return "ERROR invalid account";
        }

        // Store in session.
        $_SESSION['mediamosa_app_dbus']['username'] = $param;
        $_SESSION['mediamosa_app_dbus']['challenge'] = md5(microtime() . self::get_secret_phrase());
        return 'DATA ' . self::APP_AUTH_CONTEXT . ' 0 ' . $_SESSION['mediamosa_app_dbus']['challenge'];

      case 'DATA':
        $cookie = self::get_shared_key($_SESSION['mediamosa_app_dbus']['username']);
        list($random, $response) = explode(' ', $param, 2);
        return ($response == sha1($_SESSION['mediamosa_app_dbus']['challenge'] . ':' . $random . ':' . $cookie));
    }

    return 'ERROR syntax error';
  }

  /**
   * Process the incoming DBUS line.
   *
   * @param string $line
   *   The incoming DBUS line.
   *
   * @return string
   *   The DBus result string.
   */
  public static function app_dbus_protocol($line) {
    // We could extend with more than one authentication method if SHA1 might
    // no longer be safe enough.
    $auth_methods = array(
      'DBUS_COOKIE_SHA1' => 'dbus_cookie_sha1',
    );

    // Make sure session is clean.
    if (!isset($_SESSION['mediamosa_app_dbus'])) {
      $_SESSION['mediamosa_app_dbus'] = array(
        'mechanism' => FALSE,
        'challenge' => FALSE,
        'username' => '',
        'app_id' => 0,
      );
    }

    if (strpos($line, ' ') === FALSE) {
      return 'ERROR unknown error';
    }

    // Parse and handle command.
    list($command, $param) = explode(' ', trim($line), 2);

    switch ($command) {
      case 'AUTH':
        if ($param == '') {
          // Reject and return space speparated available methods.
          return 'REJECTED ' . implode(' ', array_keys($auth_methods));
        }

        list($mechanism, $init) = explode(' ', $param, 2);
        if (!isset($auth_methods[$mechanism])) {
          return 'ERROR unknown mechanism';
        }

        $_SESSION['mediamosa_app_dbus']['mechanism'] = $mechanism;
        $_SESSION['mediamosa_app_dbus']['username'] = FALSE;
        $_SESSION['mediamosa_app_dbus']['challenge'] = FALSE;
        $_SESSION['mediamosa_app_dbus']['app_id'] = 0;
        $param = $init;

      case 'DATA':
        // Test if we already been identified (by dbus).
        if (!empty($_SESSION['mediamosa_app_dbus']['app_id'])) {
          return 'ERROR already identified';
        }

        // Check if chosen mechanism is available.
        if (empty($_SESSION['mediamosa_app_dbus']['mechanism']) || !isset($auth_methods[$_SESSION['mediamosa_app_dbus']['mechanism']])) {
          return 'ERROR mechanism not specified';
        }

        // Call the chosen method.
        $result = call_user_func_array(
          array(__CLASS__, $auth_methods[$_SESSION['mediamosa_app_dbus']['mechanism']]),
          array($command, $param)
        );

        // When a string is returned, we do the same.
        if (is_string($result)) {
          return $result;
        }

        // FALSE means failure, that does not mean app must be logged out.
        if (!$result) {
          $_SESSION['mediamosa_app_dbus']['mechanism'] = FALSE;
          $_SESSION['mediamosa_app_dbus']['challenge'] = FALSE;
          return 'REJECTED';
        }

        // App found?
        $app = mediamosa_app_dbus::get_app($_SESSION['mediamosa_app_dbus']['username']);
        if (!$app) {
          unset($_SESSION['mediamosa_app_dbus']);
          return 'ERROR database error';
        }

        // Store the identification in session.
        self::set_session_app_id($app[mediamosa_app_db::APP_ID]);
        return 'OK ' . self::APP_AUTH_SERVER_GUID;

      case 'BEGIN':
        return mediamosa_app::get_session_app_id() ? mediamosa_response::SUCCESS : 'ERROR not identified';

      case 'CANCEL':
        unset($_SESSION['mediamosa_app_dbus']);
        return 'REJECTED';

      case 'ERROR':
        break;
    }

    return 'ERROR unknown error';
  }

  /**
   * Return the DBUS session app.
   *
   * @return integer
   *   The client application ID stored in dbus session.
   */
  public static function get_session_app_id() {
    return empty($_SESSION['mediamosa_app_dbus']['app_id']) ? FALSE : $_SESSION['mediamosa_app_dbus']['app_id'];
  }

  /**
   * Store the application ID to keep us logged in.
   *
   * @param integer $app_id
   *   The client application.
   */
  protected static function set_session_app_id($app_id) {
    // Store in global authentication.
    mediamosa_app::set_session_app_id($app_id);

    // Store so when know it was dbus.
    $_SESSION['mediamosa_app_dbus']['app_id'] = $app_id;
  }
}
